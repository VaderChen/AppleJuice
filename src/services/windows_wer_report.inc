// WER 報告 API（wer.dll，ext-ms-win-wer-reporting）與 kernelbase 的 QuirkIsEnabled（由 windows_system_objects.cpp 引入）。
// 報告物件（HREPORT）保存事件類型、P0～P9 參數、附加檔案、傾印要求與 UI 文字，並做與 Windows 相同的參數檢查；沙箱不產生錯誤報告
// （與以原則停用 WER 相同，見下方 WER 註冊的說明），WerReportSubmit 回 S_OK 並以 WerDisabled（5）表示沒有排入佇列或上傳。
// QuirkIsEnabled：本相容層沒有應用程式相容性修正（quirk）資料庫，任何 quirk 都未啟用。
if(symbol=="QuirkIsEnabled")return finish(0);
if(symbol=="WerAddExcludedApplication" || symbol=="WerRemoveExcludedApplication"){
    // (pwzExeName, bAllUsers)：排除清單為 HKCU（bAllUsers 時 HKLM）\Software\Microsoft\Windows\Windows Error Reporting\ExcludedApplications 下
    // 以執行檔名稱（路徑取最後一段）為名的 REG_DWORD 1；移除不存在的項目回 ERROR_FILE_NOT_FOUND。
    if(!arg(0))return finish(0x80070057U);auto name=read_utf16(memory,GuestAddress{arg(0)},261);if(name.empty() || name.size()>260)return finish(0x80070057U);
    if(const auto slash=name.find_last_of(u"\\/");slash!=name.npos)name=name.substr(slash+1);if(name.empty())return finish(0x80070057U);
    GuestScratch scratch(*this,memory,state);const auto key=scratch.text(u"Software\\Microsoft\\Windows\\Windows Error Reporting\\ExcludedApplications"),value=scratch.text(name),data=scratch.allocate(4);
    if(!key || !value || !data)return finish(0x8007000eU);memory.store(GuestAddress{data},32,1);
    const std::uint64_t root=arg(1)?(x86_?0x80000002U:0xffffffff80000002ULL):(x86_?0x80000001U:0xffffffff80000001ULL);
    const auto error=static_cast<std::uint32_t>(symbol=="WerAddExcludedApplication"?nested_service(state,memory,"advapi32.dll","RegSetKeyValueW",{root,key,value,4,data,4})
        :nested_service(state,memory,"advapi32.dll","RegDeleteKeyValueW",{root,key,value}));
    return finish(error?0x80070000U|(error&0xffffU):0);
}
if(symbol.starts_with("WerReport")){
    constexpr std::uint32_t invalid=0x80070057U,bad_handle=0x80070006U,full=0x8007000eU;
    const auto text=[&](unsigned index,std::size_t limit)->std::optional<std::u16string>{if(!arg(index))return std::nullopt;auto value=read_utf16(memory,GuestAddress{arg(index)},limit+1);
        if(value.size()>limit)return std::nullopt;return value;};
    if(symbol=="WerReportCreate"){
        // (pwzEventType, repType, pReportInformation, phReportHandle)：事件類型 1～64 字元；類型 WerReportNonCritical～WerReportKernel；
        // WER_REPORT_INFORMATION 的 dwSize 至少為基本版本（後續版本在尾端加欄位）。
        if(!arg(3))return finish(invalid);memory.check(GuestAddress{arg(3)},unit,Access::Write);memory.store(GuestAddress{arg(3)},bits,0);
        const auto type=text(0,64);if(!type || type->empty() || u32(1)>4)return finish(invalid);
        if(arg(2)){const auto size=memory.load(GuestAddress{arg(2)},32);const std::uint32_t base=x86_?2196U:2208U;if(size<base || size>65536)return finish(invalid);memory.check(GuestAddress{arg(2)},size,Access::Read);}
        if(wer_reports_.size()>=4096)return finish(full);
        const auto handle=gui_.next_object++;wer_reports_.emplace(handle,WerReport{*type,u32(1)});memory.store(GuestAddress{arg(3)},bits,handle);return finish(0);
    }
    const auto found=wer_reports_.find(arg(0));if(!arg(0) || found==wer_reports_.end())return finish(bad_handle);auto& report=found->second;
    if(symbol=="WerReportCloseHandle"){wer_reports_.erase(found);return finish(0);}
    if(symbol=="WerReportSetParameter"){
        // (hReport, dwparamID 0～9, pwzName 可省略, pwzValue)：名稱與值最長 WER_MAX_PARAM_LENGTH（260）字元。
        const auto id=u32(1);const auto value=text(3,260);if(id>=10 || !value)return finish(invalid);
        std::u16string name;if(arg(2)){const auto given=text(2,260);if(!given)return finish(invalid);name=*given;}
        report.names[id]=std::move(name);report.values[id]=*value;report.parameters|=1U<<id;return finish(0);
    }
    if(symbol=="WerReportAddFile"){
        // (hReport, pwzPath, repFileType 1～9, dwFileFlags：刪除／匿名／已壓縮)
        const auto path=text(1,260);if(!path || path->empty() || u32(2)<1 || u32(2)>9 || (u32(3)&~7U))return finish(invalid);
        if(report.files.size()>=512)return finish(full);report.files.emplace_back(*path,u32(2));return finish(0);
    }
    if(symbol=="WerReportAddDump"){
        // (hReport, hProcess, hThread, dumpType 1～4, pExceptionParam, pDumpCustomOptions, dwFlags：WER_DUMP_NOHEAP_ONQUEUE｜WER_DUMP_AUXILIARY)
        if(!arg(1) || u32(3)<1 || u32(3)>4 || (u32(6)&~3U))return finish(invalid);
        if(arg(4))memory.check(GuestAddress{arg(4)},unit*2,Access::Read);if(arg(5))memory.check(GuestAddress{arg(5)},4,Access::Read);
        ++report.dumps;return finish(0);
    }
    if(symbol=="WerReportSetUIOption"){
        const auto value=text(2,1024);if(u32(1)<1 || u32(1)>11 || !value)return finish(invalid);report.options[u32(1)]=*value;return finish(0);
    }
    // WerReportSubmit(hReport, consent 1～4, dwFlags, pSubmitResult)
    if(u32(1)<1 || u32(1)>4 || (u32(2)&~0xffffU))return finish(invalid);
    if(arg(3)){memory.check(GuestAddress{arg(3)},4,Access::Write);memory.store(GuestAddress{arg(3)},32,5);}
    return finish(0);
}
