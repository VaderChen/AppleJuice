// 硬連結列舉 FindFirstFileNameW／FindNextFileNameW（kernel32 與 kernelbase 同一實作）：
// 名稱是不含磁碟代號的磁碟區相對路徑（\dir\file）；查詢的路徑排第一，連結數大於 1 時在同一磁碟機的宿主目錄內依 inode 找出其他連結
// （不跟隨符號連結，最多走訪 500000 個項目）。搜尋 handle 與 FindFirstFile／FindFirstStreamW 共用 directory_searches，以 FindClose 關閉。
// 緩衝區不足時回 ERROR_MORE_DATA 並以 StringLength 回報所需字元數（含結尾 NUL）；列舉完畢 ERROR_HANDLE_EOF。
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <filesystem>
#include <sys/stat.h>

namespace x64arm {
std::span<const WindowsServiceDescriptor> file_link_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"kernel32.dll","FindFirstFileNameW",4,0,0,0x2},{"kernel32.dll","FindNextFileNameW",3},
        {"kernelbase.dll","FindFirstFileNameW",4,0,0,0x2},{"kernelbase.dll","FindNextFileNameW",3}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_file_links(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);
    const auto invalid=x86_?UINT64_C(0xffffffff):UINT64_MAX;
    const auto fail=[&](std::uint32_t error,std::uint64_t value){last_error_=error;arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    // 名稱寫入 (StringLength*, LinkName)；不足時回報所需長度。
    const auto deliver=[&](std::uint64_t length_at,std::uint64_t buffer,const std::string& name)->std::uint32_t{
        const auto text=utf8_to_utf16(name);const auto needed=static_cast<std::uint32_t>(text.size()+1);
        const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{length_at},32));
        memory.store(GuestAddress{length_at},32,needed);if(!buffer || capacity<needed)return 234;
        memory.write(GuestAddress{buffer},utf16_bytes(text));return 0;
    };
    try{
        if(service.symbol=="FindNextFileNameW"){
            // (hFindStream, StringLength*, LinkName)
            const auto found=runtime_.directory_searches.find(arg(0));if(found==runtime_.directory_searches.end() || !found->second.names)return fail(6,0);
            if(!arg(1))return fail(87,0);auto& search=found->second;if(search.cursor>=search.entries.size())return fail(38,0);
            if(const auto error=deliver(arg(1),arg(2),search.entries[search.cursor].name))return fail(error,0);
            ++search.cursor;last_error_=0;arg.return_to(state,1);return std::nullopt;
        }
        // FindFirstFileNameW(lpFileName, dwFlags, StringLength*, LinkName)
        if(!arg(0) || static_cast<std::uint32_t>(arg(1)) || !arg(2))return fail(87,invalid);
        if(runtime_.directory_searches.size()>=1024)return fail(8,invalid);
        auto path=utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)},32768));
        for(auto& c:path)if(c=='/')c='\\';
        if(path.size()<3 || path[1]!=':' || path[2]!='\\'){
            // 相對路徑以目前目錄展開。
            const auto current=utf16_to_utf8(runtime_.current_directory);if(current.empty())return fail(3,invalid);
            path=current+(current.back()=='\\'?"":"\\")+path;
        }
        const auto [host,error]=filesystem_.unix_file_name(path);if(error || host.empty())return fail(error?error:2,invalid);
        struct stat target{};if(::lstat(host.c_str(),&target))return fail(errno==ENOENT?2U:5U,invalid);
        std::vector<std::string> names;
        const auto volume_relative=[](std::string guest)->std::string{if(guest.size()>=2 && guest[1]==':')guest.erase(0,2);if(guest.empty() || guest.front()!='\\')guest.insert(guest.begin(),'\\');return guest;};
        {const auto [own,own_error]=filesystem_.dos_file_name(host);names.push_back(volume_relative(own_error || own.empty()?path:own));}
        if(target.st_nlink>1){
            const auto [root,root_error]=filesystem_.unix_file_name(path.substr(0,3));
            if(!root_error && !root.empty()){
                std::error_code walk_error;std::size_t visited=0;
                for(std::filesystem::recursive_directory_iterator it(root,std::filesystem::directory_options::skip_permission_denied,walk_error),end;it!=end && !walk_error && ++visited<=500000;it.increment(walk_error)){
                    struct stat entry{};if(::lstat(it->path().c_str(),&entry) || entry.st_ino!=target.st_ino || entry.st_dev!=target.st_dev || it->path()==host)continue;
                    const auto [guest,guest_error]=filesystem_.dos_file_name(it->path());if(guest_error || guest.empty())continue;
                    names.push_back(volume_relative(guest));if(names.size()>=static_cast<std::size_t>(target.st_nlink))break;
                }
            }
        }
        if(const auto status=deliver(arg(2),arg(3),names.front()))return fail(status,invalid);
        DirectorySearch search;search.names=true;search.cursor=1;
        for(auto& name:names){DirectoryEntry entry;entry.name=std::move(name);search.entries.push_back(std::move(entry));}
        const auto handle=runtime_.allocate_wait_handle();runtime_.directory_searches.emplace(handle,std::move(search));
        last_error_=0;arg.return_to(state,handle);return std::nullopt;
    }catch(const MemoryFault&){return fail(998,service.symbol=="FindNextFileNameW"?0:invalid);}
}
}
