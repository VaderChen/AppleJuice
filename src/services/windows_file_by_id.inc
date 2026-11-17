// OpenFileById（由 windows_files.cpp 的 invoke_file 引入）：(hVolumeHint, lpFileId, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwFlagsAndAttributes)。
// 檔案 ID 即 GetFileInformationByHandle／FileIdInfo 回報的宿主 inode：以提示 handle 所在的宿主檔案系統找回路徑（fsgetpath），
// 再經沙箱映射換回客體路徑（不在映射內的檔案視為不存在），最後沿用 CreateFileW 開啟（OPEN_EXISTING），存取檢查與錯誤碼都與一般開啟相同。
// FILE_ID_DESCRIPTOR {dwSize, Type, {FileId | ObjectId | ExtendedFileId}}：FileIdType 0 與 ExtendedFileIdType 2（高 64 位元須為 0）；沒有物件 ID。
if(service.symbol=="OpenFileById"){
    const WindowsCall call(state,memory,service);
    const auto fail=[&](std::uint32_t error){last_error_=error;call.return_to(state,x86_?UINT32_MAX:UINT64_MAX);return std::optional<std::uint32_t>{};};
    try{
        const auto hint=files_.find(call(0));if(hint==files_.end())return fail(6);
        if(!call(1) || hint->second.path.empty())return fail(87);
        const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{call(1)},32)),type=static_cast<std::uint32_t>(memory.load(GuestAddress{call(1)+4},32));
        if(size<24)return fail(87);
        std::uint64_t id=0;
        if(type==0)id=memory.load(GuestAddress{call(1)+8},64);
        else if(type==2){id=memory.load(GuestAddress{call(1)+8},64);if(memory.load(GuestAddress{call(1)+16},64))return fail(2);}
        else if(type==1)return fail(2);
        else return fail(87);
#if defined(__APPLE__)
        const auto volume=filesystem_.unix_file_name(hint->second.path);if(volume.second)return fail(volume.second);
        struct statfs information{};if(::statfs(volume.first.c_str(),&information)!=0)return fail(2);
        std::vector<char> buffer(4*PATH_MAX);const auto length=::fsgetpath(buffer.data(),buffer.size(),&information.f_fsid,id);
        if(length<=0)return fail(2);
        const auto guest=filesystem_.dos_file_name(std::filesystem::path(std::string(buffer.data())));if(guest.second)return fail(2);
        GuestScratch scratch(*this,memory,state);const auto text=scratch.text(utf8_to_utf16(guest.first));if(!text)return fail(8);
        call.return_to(state,nested_service(state,memory,"kernel32.dll","CreateFileW",{text,call(2),call(3),call(4),3,call(5),0}));return std::nullopt;
#else
        return fail(50);
#endif
    }catch(const MemoryFault&){return fail(998);}catch(const std::bad_alloc&){return fail(8);}
}
