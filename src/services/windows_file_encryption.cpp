#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"

namespace x64arm {
std::span<const WindowsServiceDescriptor> file_encryption_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
        {"advapi32.dll","DecryptFileW",2,0,0,2},{"advapi32.dll","DecryptFileA",2,0,0,2},
        {"advapi32.dll","FileEncryptionStatusW",2},{"advapi32.dll","FileEncryptionStatusA",2}
    };return entries;
}

std::optional<std::uint32_t> WindowsServices::invoke_file_encryption(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const bool decrypt=service.symbol.starts_with("DecryptFile");
    const auto finish=[&](bool success,std::uint32_t error=0){if(!success)last_error_=error;arg.return_to(state,success?1:0);return std::optional<std::uint32_t>{};};
    try{
        if(!arg(0) || (decrypt?arg(1)!=0:arg(1)==0))return finish(false,87);
        if(!decrypt)memory.check(GuestAddress{arg(1)},4,Access::Write);
        const auto path=service.symbol.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)})):read_ansi(memory,arg(0));
        const auto info=filesystem_.target_information(path);if(info.error)return finish(false,info.error);
        const auto attributes=info.metadata.attributes;
        if(!decrypt){
            std::uint32_t status=6; // FILE_SYSTEM_NOT_SUPPORT：映射磁碟沒有啟用 EFS。
            if(attributes&0x4000U)status=1;
            else if(attributes&1U)status=8;
            else if(attributes&4U)status=attributes&0x10U?4U:2U;
            else if(attributes&0x10U){const auto location=filesystem_.volume_location(path,true);if(!location.error && location.path.empty())status=3;}
            memory.store(GuestAddress{arg(1)},32,status);return finish(true);
        }
        if(attributes&1U)return finish(false,6009); // ERROR_FILE_READ_ONLY。
        if(attributes&0x4000U)return finish(false,6004); // 沒有 EFS driver 時不可假裝解密加密檔。
        if(!filesystem_.writable())return finish(false,5);
        constexpr std::uint32_t required_access=0x100183; // DATA、ATTRIBUTES 讀寫及 SYNCHRONIZE。
        if(const auto error=filesystem_.security_access(path,required_access))return finish(false,error);
        // 仍以共用檔案層取得 exclusive open，驗證 Windows token、共享及路徑邊界。
        // 目錄的 DATA 權限已核對；宿主目錄以 metadata descriptor 開啟，不寫入其內容。
        const bool directory=(attributes&0x10U)!=0;
        const auto opened=filesystem_.open_host(path,directory?(required_access&~3U):required_access,3,directory?0x02000000U:0U,0);
        if(opened.error)return finish(false,opened.error);if(!opened.file)return finish(false,6);
        const auto current=opened.file->information();if(current.error)return finish(false,current.error);
        if(current.metadata.attributes&1U)return finish(false,6009);
        if(current.metadata.attributes&0x4000U)return finish(false,6004);
        // Microsoft 的契約：已未加密的檔案／目錄直接成功，保留原內容及 metadata。
        return finish(true);
    }catch(const MemoryFault&){return finish(false,998);}catch(const std::invalid_argument&){return finish(false,123);}catch(const std::bad_alloc&){return finish(false,8);}
}
}
