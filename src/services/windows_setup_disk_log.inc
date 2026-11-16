// setupapi 磁碟空間清單、安裝記錄檔、檔案記錄與非互動模式（由 windows_setup_inf.cpp 的 invoke_setup_inf 引入）。
// 磁碟空間清單：依完整路徑記錄複製（FILEOP_COPY，含檔案大小）或刪除（FILEOP_DELETE）；查詢時大小依該磁碟的配置單位進位，
// 複製扣除、刪除釋放既有檔案佔用的空間（SPDSL_IGNORE_DISK 時不看既有檔案）。安裝記錄寫入 %windir%\setupact.log，錯誤以上同時寫入 setuperr.log。
// 檔案記錄是 INI 格式："目標"="來源","檢查碼","磁片說明","磁片標記檔","其他資訊"，預設區段 Files.WinNt；系統記錄為 %windir%\repair\setup.log（需系統管理員）。
{
    const auto u32=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    const auto windows_directory=[&]{auto value=environment_value(u"windir");return value.empty()?std::u16string(u"C:\\Windows"):value;};
    const auto write_whole=[&](const std::u16string& path,const std::string& content){
        auto opened=filesystem_.open_host(utf16_to_utf8(path),0x40000000U,2,0x80,3);if(opened.error)throw Failure{opened.error};
        const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(content.data()),content.size());
        const auto written=opened.file->write(bytes,0);if(written.error)throw Failure{written.error};if(const auto error=opened.file->truncate(content.size()))throw Failure{error};};
    const auto append=[&](const std::u16string& path,const std::string& content){
        auto opened=filesystem_.open_host(utf16_to_utf8(path),0x40000000U,4,0x80,3);if(opened.error)throw Failure{opened.error};
        const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(content.data()),content.size());
        const auto written=opened.file->write(bytes,opened.file->current_size());if(written.error)throw Failure{written.error};};
    const auto read_whole=[&](const std::u16string& path)->std::optional<std::string>{
        auto opened=filesystem_.open_reader(utf16_to_utf8(path));if(opened.error==2 || opened.error==3)return std::nullopt;if(opened.error)throw Failure{opened.error};
        if(opened.metadata.size>16*1024*1024)throw Failure{1006};std::string content(static_cast<std::size_t>(opened.metadata.size),'\0');
        const auto read=opened.file->read(std::span(reinterpret_cast<std::uint8_t*>(content.data()),content.size()),0);if(read.error)throw Failure{read.error};content.resize(static_cast<std::size_t>(read.bytes));return content;};
    const auto store_text=[&](std::uint64_t address,std::u16string_view value){
        if(wide){memory.write(GuestAddress{address},utf16_bytes(std::u16string(value)));return;}
        const auto narrow=utf16_to_utf8(value);std::vector<std::uint8_t> bytes(narrow.begin(),narrow.end());bytes.push_back(0);memory.write(GuestAddress{address},bytes);};
    const auto text_length=[&](std::u16string_view value)->std::size_t{return wide?value.size():utf16_to_utf8(value).size();};
    const auto handle_block=[&]{const auto handle=allocate_heap(memory,16);if(!handle)throw std::bad_alloc{};return handle;};
    if(name.find("DiskSpaceList")!=std::string_view::npos || name.starts_with("SetupQuerySpaceRequiredOnDrive")){
        auto& lists=setup_inf_->disk_spaces;
        const auto list=[&](std::uint64_t handle)->State::DiskSpace&{const auto found=lists.find(handle);if(found==lists.end())throw Failure{6};return found->second;};
        const auto drive_of=[](std::u16string_view path)->char16_t{if(path.size()<2 || path[1]!=u':')return 0;auto letter=path[0];if(letter>=u'a' && letter<=u'z')letter=static_cast<char16_t>(letter-32);return letter>=u'A' && letter<=u'Z'?letter:0;};
        if(name.starts_with("SetupCreateDiskSpaceList")){
            // (Reserved1, Reserved2, Flags)：SPDSL_IGNORE_DISK（1）、SPDSL_DISALLOW_NEGATIVE_ADJUST（2）。
            if(arg(0) || u32(1) || (u32(2)&~3U))throw Failure{87};if(lists.size()>=65536)throw std::bad_alloc{};
            const auto handle=handle_block();lists[handle].flags=u32(2);return succeed(handle);
        }
        if(name.starts_with("SetupDuplicateDiskSpaceList")){
            auto copy=list(arg(0));if(arg(1) || u32(2) || (u32(3)&~3U))throw Failure{87};copy.flags=u32(3);const auto handle=handle_block();lists[handle]=std::move(copy);return succeed(handle);
        }
        if(name=="SetupDestroyDiskSpaceList"){(void)list(arg(0));lists.erase(arg(0));(void)free_heap(memory,arg(0),state);return succeed();}
        if(name.starts_with("SetupAddToDiskSpaceList") || name.starts_with("SetupRemoveFromDiskSpaceList")){
            // Add(List, TargetFilespec, LONGLONG FileSize, Operation, Reserved1, Reserved2)；Remove(List, TargetFilespec, Operation, Reserved1, Reserved2)。
            const bool add=name.starts_with("SetupAddToDiskSpaceList");auto& target=list(arg(0));const auto spec=read_text(arg(1));if(spec.empty())throw Failure{87};
            const auto operation=u32(add?3:2);if((operation!=0 && operation!=2) || arg(add?4:3) || u32(add?5:4))throw Failure{87};
            const auto path=full_path(spec);if(!drive_of(path))throw Failure{15};const auto key=fold(path);
            if(add){const auto size=static_cast<std::int64_t>(arg(2));if(size<0)throw Failure{87};target.files[key]={path,size,operation};return succeed();}
            const auto found=target.files.find(key);if(found==target.files.end() || found->second.operation!=operation)throw Failure{2};target.files.erase(found);return succeed();
        }
        if(name.starts_with("SetupQuerySpaceRequiredOnDrive")){
            // (List, DriveSpec, LONGLONG* SpaceRequired, Reserved1, Reserved2)：清單中沒有此磁碟時回報 ERROR_INVALID_DRIVE。
            const auto& target=list(arg(0));const auto drive=drive_of(read_text(arg(1)));if(!drive)throw Failure{15};if(!arg(2) || arg(3) || u32(4))throw Failure{87};
            const auto root=std::u16string(1,drive)+u":\\";const auto space=filesystem_.volume_space(utf16_to_utf8(root));
            const std::int64_t cluster=space.error || !space.allocation_unit?4096:space.allocation_unit;const auto rounded=[&](std::int64_t value){return (value+cluster-1)/cluster*cluster;};
            bool any=false;std::int64_t total=0;
            for(const auto& [key,file]:target.files){(void)key;if(drive_of(file.path)!=drive)continue;any=true;std::int64_t existing=0;
                if(!(target.flags&1U)){const auto info=filesystem_.information(utf16_to_utf8(file.path));if(!info.error && !(info.metadata.attributes&0x10U))existing=rounded(static_cast<std::int64_t>(info.metadata.size));}
                total+=(file.operation==0?rounded(file.size):0)-existing;}
            if(!any)throw Failure{15};memory.store(GuestAddress{arg(2)},64,static_cast<std::uint64_t>(total));return succeed();
        }
        if(name.starts_with("SetupQueryDrivesInDiskSpaceList")){
            // (List, ReturnBuffer, ReturnBufferSize, RequiredSize)：以 "C:\0D:\0\0" 回傳；ReturnBuffer 為 NULL 時只回報需要的長度。
            const auto& target=list(arg(0));std::set<char16_t> drives;for(const auto& [key,file]:target.files){(void)key;drives.insert(drive_of(file.path));}
            std::u16string listing;for(const auto letter:drives){listing+=letter;listing+=u':';listing+=u'\0';}listing+=u'\0';
            if(arg(3))memory.store(GuestAddress{arg(3)},32,listing.size());if(!arg(1))return succeed();if(u32(2)<listing.size())throw Failure{122};
            if(wide)memory.write(GuestAddress{arg(1)},std::span(reinterpret_cast<const std::uint8_t*>(listing.data()),listing.size()*2));
            else{std::vector<std::uint8_t> bytes(listing.begin(),listing.end());memory.write(GuestAddress{arg(1)},bytes);}
            return succeed();
        }
    }
    if(name=="SetupOpenLog" || name=="SetupCloseLog" || name.starts_with("SetupLogError")){
        const auto action=windows_directory()+u"\\setupact.log",errors=windows_directory()+u"\\setuperr.log";
        if(name=="SetupOpenLog"){
            // (Erase)：建立或清空兩個記錄檔。
            for(const auto& path:{action,errors}){if(arg(0)&0xff)write_whole(path,{});else append(path,{});}setup_inf_->log_open=true;return succeed();
        }
        if(name=="SetupCloseLog"){setup_inf_->log_open=false;return finish(0);}
        // (MessageString, Severity)：LogSevInformation 0～LogSevFatalError 3；訊息原樣寫入（ANSI 以 UTF-8）。
        if(!setup_inf_->log_open)throw Failure{1006};const auto severity=u32(1);if(severity>3)throw Failure{87};
        const auto message=utf16_to_utf8(read_text(arg(0)));append(action,message);if(severity>=2)append(errors,message);return succeed();
    }
    if(name.starts_with("SetupInitializeFileLog") || name=="SetupTerminateFileLog" || name.starts_with("SetupLogFile") || name.starts_with("SetupQueryFileLog") || name.starts_with("SetupRemoveFileLogEntry")){
        auto& logs=setup_inf_->file_logs;
        const auto log=[&](std::uint64_t handle)->State::FileLog&{const auto found=logs.find(handle);if(found==logs.end())throw Failure{6};return found->second;};
        const auto section_name=[&](std::uint64_t address){auto value=read_text(address);return value.empty()?std::u16string(u"Files.WinNt"):value;};
        const auto find_section=[&](State::FileLog& file,std::u16string_view section)->State::FileLog::Section*{
            for(auto& item:file.sections)if(fold(item.name)==fold(std::u16string(section)))return &item;return nullptr;};
        const auto serialize=[&](const State::FileLog& file){
            std::u16string out;
            for(const auto& section:file.sections){out+=u"["+section.name+u"]\r\n";
                for(const auto& entry:section.entries){out+=u"\""+entry.target+u"\"=";for(std::size_t i=0;i<entry.fields.size();++i){if(i)out+=u',';out+=u"\""+entry.fields[i]+u"\"";}out+=u"\r\n";}}
            return utf16_to_utf8(out);};
        const auto parse=[&](std::string_view content){
            std::vector<State::FileLog::Section> sections;const auto text=utf8_to_utf16(content);std::size_t at=0;
            const auto clean=[](std::u16string value){std::erase(value,u'"');return value;};
            while(at<text.size()){
                auto end=text.find(u'\n',at);if(end==std::u16string::npos)end=text.size();auto line=text.substr(at,end-at);at=end+1;
                while(!line.empty() && (line.back()==u'\r' || line.back()==u' '))line.pop_back();if(line.empty())continue;
                if(line.front()==u'['){const auto close=line.find(u']');if(close!=std::u16string::npos)sections.push_back({line.substr(1,close-1),{}});continue;}
                if(sections.empty())continue;
                std::size_t equals;std::u16string target;
                if(line.front()==u'"'){const auto close=line.find(u'"',1);if(close==std::u16string::npos)continue;target=line.substr(1,close-1);equals=line.find(u'=',close);}
                else{equals=line.find(u'=');if(equals==std::u16string::npos)continue;target=line.substr(0,equals);}
                if(equals==std::u16string::npos)continue;
                std::vector<std::u16string> fields;std::size_t position=equals+1;
                while(position<=line.size()){const auto comma=line.find(u',',position);fields.push_back(clean(line.substr(position,comma==std::u16string::npos?std::u16string::npos:comma-position)));if(comma==std::u16string::npos)break;position=comma+1;}
                fields.resize(5);sections.back().entries.push_back({target,std::move(fields)});
            }
            return sections;};
        const auto save=[&](const State::FileLog& file){if(!(file.flags&4U))write_whole(file.path,serialize(file));};
        if(name.starts_with("SetupInitializeFileLog")){
            // (LogFileName, Flags)：SPFILELOG_SYSTEMLOG 1（檔名須為 NULL）、SPFILELOG_FORCENEW 2、SPFILELOG_QUERYONLY 4；失敗回傳 INVALID_HANDLE_VALUE。
            try{
                const auto flags=u32(1);if((flags&~7U) || ((flags&2U) && (flags&4U)))throw Failure{87};const bool system=flags&1U;
                if(system==(arg(0)!=0))throw Failure{87};
                if(system){initialize_tokens();const auto& token=runtime_.token_objects.at(effective_token_id());const auto administrators=sddl_sid("BA");
                    if(!security::token_sid_matches(token,administrators,false) || (token.restriction_check && !security::token_sid_matches(token,administrators,false,true)))throw Failure{5};}
                State::FileLog file;file.path=system?windows_directory()+u"\\repair\\setup.log":full_path(read_text(arg(0)));file.flags=flags;
                const auto content=(flags&2U)?std::optional<std::string>{}:read_whole(file.path);
                if(!content && (flags&4U))throw Failure{2};
                if(content)file.sections=parse(*content);
                if(!(flags&4U)){if(flags&2U)write_whole(file.path,{});else append(file.path,{});}
                if(logs.size()>=4096)throw std::bad_alloc{};const auto handle=handle_block();logs[handle]=std::move(file);return succeed(handle);
            }catch(const Failure& error){return fail(error.code,invalid_handle);}
        }
        if(name=="SetupTerminateFileLog"){auto& file=log(arg(0));save(file);logs.erase(arg(0));(void)free_heap(memory,arg(0),state);return succeed();}
        if(name.starts_with("SetupLogFile")){
            // (Log, LogSectionName, SourceFilename, TargetFilename, Checksum, DiskTagfile, DiskDescription, OtherInfo, Flags)：同一目標取代舊項目。
            auto& file=log(arg(0));if(file.flags&4U)throw Failure{5};if(u32(8)&~1U)throw Failure{87};
            const auto source=read_text(arg(2)),target=read_text(arg(3));if(source.empty() || target.empty())throw Failure{87};
            const auto section=section_name(arg(1));auto* found=find_section(file,section);if(!found){file.sections.push_back({section,{}});found=&file.sections.back();}
            char checksum[9];std::snprintf(checksum,sizeof checksum,"%08x",u32(4));
            std::vector<std::u16string> fields{source,utf8_to_utf16(checksum),read_text(arg(6)),read_text(arg(5)),read_text(arg(7))};for(auto& field:fields)std::erase(field,u'"');
            auto key=target;std::erase(key,u'"');
            const auto existing=std::find_if(found->entries.begin(),found->entries.end(),[&](const auto& entry){return fold(entry.target)==fold(key);});
            if(existing!=found->entries.end())existing->fields=std::move(fields);else found->entries.push_back({key,std::move(fields)});
            save(file);return succeed();
        }
        if(name.starts_with("SetupQueryFileLog")){
            // (Log, LogSectionName, TargetFilename, DesiredInfo, DataOut, ReturnBufferSize, RequiredSize)：DesiredInfo 0 來源、1 檢查碼、2 磁片標記檔、3 磁片說明、4 其他資訊。
            auto& file=log(arg(0));const auto desired=u32(3);if(desired>4)throw Failure{87};const auto target=read_text(arg(2));if(target.empty())throw Failure{87};
            auto* section=find_section(file,section_name(arg(1)));if(!section)throw Failure{2};
            const auto found=std::find_if(section->entries.begin(),section->entries.end(),[&](const auto& entry){return fold(entry.target)==fold(target);});if(found==section->entries.end())throw Failure{2};
            constexpr unsigned order[5]{0,1,3,2,4};const auto& value=found->fields[order[desired]];const auto needed=text_length(value)+1;
            if(arg(6))memory.store(GuestAddress{arg(6)},32,needed);if(!arg(4))return succeed();if(u32(5)<needed)throw Failure{122};store_text(arg(4),value);return succeed();
        }
        // SetupRemoveFileLogEntry(Log, LogSectionName, TargetFilename)：TargetFilename 為 NULL 時移除整個區段。
        auto& file=log(arg(0));if(file.flags&4U)throw Failure{5};const auto section_text=section_name(arg(1));auto* section=find_section(file,section_text);if(!section)throw Failure{2};
        if(!arg(2)){std::erase_if(file.sections,[&](const auto& item){return fold(item.name)==fold(section_text);});}
        else{const auto target=fold(read_text(arg(2)));const auto before=section->entries.size();std::erase_if(section->entries,[&](const auto& entry){return fold(entry.target)==target;});if(section->entries.size()==before)throw Failure{2};}
        save(file);return succeed();
    }
    if(name=="SetupGetNonInteractiveMode")return finish((setup_inf_->global_flags&4U)?1:0);
    if(name=="SetupSetNonInteractiveMode"){
        // 回傳先前的設定；與 pSetupGetGlobalFlags 的 PSPGF_NONINTERACTIVE（4）共用。
        const bool previous=setup_inf_->global_flags&4U;if(arg(0)&0xff)setup_inf_->global_flags|=4U;else setup_inf_->global_flags&=~4U;return finish(previous?1:0);
    }
}
