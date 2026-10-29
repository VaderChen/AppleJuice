#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_paths.hpp"
#include <algorithm>
#include <cerrno>
#include <limits>
#if !defined(_WIN32)
#include <sys/statvfs.h>
#include <unistd.h>
#endif

namespace x64arm {
namespace {
FileVolumeSpace descriptor_volume_space(int descriptor) {
#if defined(_WIN32)
    (void)descriptor;return {0, 0, 0, 0, 0, 50};
#else
    struct statvfs volume{};
    if (::fstatvfs(descriptor, &volume)) return {0, 0, 0, 0, 0, errno == EACCES ? 5U : 1117U};
    const std::uint64_t unit = volume.f_frsize ? volume.f_frsize : volume.f_bsize;
    if (!unit || unit > UINT32_MAX || volume.f_blocks > UINT64_MAX / unit) return {0, 0, 0, 0, 0, 534};
    const auto total = static_cast<std::uint64_t>(volume.f_blocks) * unit;
    const auto free = std::min(static_cast<std::uint64_t>(volume.f_bfree), static_cast<std::uint64_t>(volume.f_blocks)) * unit;
    const auto available = std::min(static_cast<std::uint64_t>(volume.f_bavail), free / unit) * unit;
    // 虛擬磁碟採 512-byte 邏輯磁區；容量來自安全開啟的檔案或目錄 fd，未洩漏宿主路徑。
    if (unit % 512) return {0, 0, 0, 0, 0, 50};
    return {total, free, available, static_cast<std::uint32_t>(unit), 512, 0};
#endif
}
}
FileVolumeSpace HostFile::volume_space() const {return descriptor_volume_space(descriptor_);}
FileVolumeSpace ReadOnlyFiles::volume_space(std::string path) {
    const auto resolved=resolve(std::move(path),true);
    if(resolved.error)return {0,0,0,0,0,resolved.error};
#if !defined(_WIN32)
    struct Descriptor {int value;~Descriptor(){if(value>=0)::close(value);}} fd{resolved.descriptor};
    return descriptor_volume_space(fd.value);
#else
    return {0,0,0,0,0,50};
#endif
}
std::span<const WindowsServiceDescriptor> volume_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"kernel32.dll","SetVolumeMountPointA",2},{"kernel32.dll","SetVolumeMountPointW",2},
        {"kernel32.dll","DeleteVolumeMountPointA",1},{"kernel32.dll","DeleteVolumeMountPointW",1},
        {"kernel32.dll","FindFirstVolumeA",2},{"kernel32.dll","FindFirstVolumeW",2},
        {"kernel32.dll","FindNextVolumeA",3},{"kernel32.dll","FindNextVolumeW",3},
        {"kernel32.dll","GetVolumeNameForVolumeMountPointA",3},{"kernel32.dll","GetVolumeNameForVolumeMountPointW",3},
        {"kernel32.dll","GetVolumePathNamesForVolumeNameA",4},{"kernel32.dll","GetVolumePathNamesForVolumeNameW",4},
        {"kernel32.dll","GetVolumePathNameA",3},{"kernel32.dll","GetVolumePathNameW",3},
        {"kernel32.dll","FindFirstVolumeMountPointA",3},{"kernel32.dll","FindFirstVolumeMountPointW",3},
        {"kernel32.dll","FindNextVolumeMountPointA",3},{"kernel32.dll","FindNextVolumeMountPointW",3},
        {"kernel32.dll","FindVolumeClose",1},{"kernel32.dll","FindVolumeMountPointClose",1},
        {"kernel32.dll","QueryDosDeviceA",3},{"kernel32.dll","QueryDosDeviceW",3},{"kernel32.dll", "GetLogicalDrives", 0},
        {"kernel32.dll", "GetLogicalDriveStringsA", 2}, {"kernel32.dll", "GetLogicalDriveStringsW", 2},
        {"kernel32.dll", "GetDriveTypeA", 1}, {"kernel32.dll", "GetDriveTypeW", 1},
        {"kernel32.dll", "GetDiskFreeSpaceA", 5}, {"kernel32.dll", "GetDiskFreeSpaceW", 5},
        {"kernel32.dll", "GetDiskFreeSpaceExA", 4}, {"kernel32.dll", "GetDiskFreeSpaceExW", 4},
        {"kernel32.dll", "GetVolumeInformationA", 8}, {"kernel32.dll", "GetVolumeInformationW", 8},
        {"kernel32.dll", "SetVolumeLabelA", 2}, {"kernel32.dll", "SetVolumeLabelW", 2},
        {"kernel32.dll", "GetCompressedFileSizeA", 2}, {"kernel32.dll", "GetCompressedFileSizeW", 2},
        {"kernel32.dll", "GetTempFileNameA", 4}, {"kernel32.dll", "GetTempFileNameW", 4},
        {"kernel32.dll", "GetTempPath2A", 2}, {"kernel32.dll", "GetTempPath2W", 2}, {"kernel32.dll", "GetTempPathA", 2}, {"kernel32.dll", "GetTempPathW", 2},
        {"kernel32.dll", "FindFirstChangeNotificationA", 3}, {"kernel32.dll", "FindFirstChangeNotificationW", 3},
        {"kernel32.dll", "ReadDirectoryChangesW", 8},
        {"kernel32.dll", "ReadDirectoryChangesExW", 9},
        {"ntdll.dll", "NtNotifyChangeDirectoryFile", 9},
        {"ntdll.dll", "NtNotifyChangeDirectoryFileEx", 10},
        {"kernel32.dll", "FindNextChangeNotification", 1}, {"kernel32.dll", "FindCloseChangeNotification", 1}
        ,{"crt","_tempnam",2},{"crt","_wtempnam",2},{"crt","tmpnam",1},{"crt","_wtmpnam",1}
        ,{"crt","tmpnam_s",2},{"crt","_wtmpnam_s",2}
    };
    return table;
}
namespace {
std::u16string temp_absolute(std::u16string path) {
    std::replace(path.begin(), path.end(), u'/', u'\\');
    std::u16string prefix = u"C:\\";
    std::size_t start = 0;
    if (path.starts_with(u"\\\\")) {
        const auto server_end = path.find(u'\\', 2);
        if (server_end == std::u16string::npos || server_end == 2)
            throw std::invalid_argument("UNC 暫存路徑缺少 server 或 share");
        const auto share_end = path.find(u'\\', server_end + 1);
        const auto root_end = share_end == std::u16string::npos ? path.size() : share_end;
        if (root_end == server_end + 1) throw std::invalid_argument("UNC 暫存路徑缺少 share");
        prefix = path.substr(0, root_end) + u'\\';
        start = root_end == path.size() ? root_end : root_end + 1;
    } else if (path.size() >= 2 && path[1] == u':') {
        if (!((path[0] >= u'A' && path[0] <= u'Z') || (path[0] >= u'a' && path[0] <= u'z')))
            throw std::invalid_argument("暫存路徑磁碟代號不合法");
        prefix[0] = path[0]; start = 2;
    }
    std::vector<std::u16string> parts;
    while (start < path.size()) {
        auto end = path.find(u'\\', start); if (end == std::u16string::npos) end = path.size();
        auto part = path.substr(start, end - start); start = end + 1;
        // UNC 的 server/share 已保存在 prefix；.. 僅能消除其下的路徑片段。
        if (part == u"..") { if (!parts.empty()) parts.pop_back(); }
        else if (!part.empty() && part != u".") parts.push_back(std::move(part));
    }
    for (const auto& part : parts) { prefix += part; prefix += u'\\'; }
    return prefix;
}
}
std::optional<std::uint32_t> WindowsServices::invoke_volume(GuestState& state, GuestMemory& memory,
                                                         const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state, memory, service);
    const auto symbol = service.symbol;
    if(symbol.starts_with("NtNotifyChangeDirectory"))return invoke_directory_notification(state,memory,service);
    const bool wide = symbol.back() == 'W' || symbol.starts_with("_wtempnam") || symbol.starts_with("_wtmpnam");
    const auto u32 = [&](unsigned i) { return static_cast<std::uint32_t>(arg(i)); };
    const auto text = [&](unsigned i) { return wide ? utf16_to_utf8(read_utf16(memory, GuestAddress{arg(i)})) : read_ansi(memory, arg(i)); };
    const auto encoded = [&](std::u16string_view value) {
        if (wide) return utf16_bytes(value);
        const auto ansi = utf16_to_utf8(value); std::vector<std::uint8_t> out(ansi.begin(), ansi.end()); out.push_back(0); return out;
    };
    std::uint64_t result = 0;
    const auto fail = [&](std::uint32_t code) { last_error_ = code; result = 0; };
    // Win32 與 CRT 使用同一份 Windows 環境與路徑解析，從不建立宿主暫存檔。
    const auto temporary_directory=[&]{
        std::u16string path=u"C:\\Windows";
        for(const auto* key:{u"TMP",u"TEMP",u"USERPROFILE"}){
            const auto found=runtime_.environment.find(key);
            if(found!=runtime_.environment.end() && !found->second.second.empty()){path=found->second.second;break;}
        }
        return temp_absolute(full_windows_environment_path(std::move(path),runtime_.current_directory,runtime_.environment));
    };
    if(service.family=="crt"){
        const bool allocated=symbol=="_tempnam" || symbol=="_wtempnam",secure=symbol.ends_with("_s");
        const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
        const auto error=[&](unsigned value){memory.store(GuestAddress{runtime_.errno_cell},32,value);return finish(secure?value:0);};
        if(secure && !arg(0)){
            memory.store(GuestAddress{runtime_.errno_cell},32,22);
            if(current_invalid_parameter_handler()){
                start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,22);
                return std::nullopt;
            }
            return 0xc0000417U;
        }
        try{
            std::u16string directory,prefix;
            const auto exists=[&](std::u16string_view path){
                const auto information=filesystem_.information(utf16_to_utf8(path));
                return !information.error && (information.metadata.attributes&0x10U);
            };
            if(allocated){
                // _tempnam 的 TMP 選擇與 tmpnam 的 GetTempPath 契約不同。
                const auto found=runtime_.environment.find(u"TMP");
                const bool has_tmp=found!=runtime_.environment.end() && !found->second.second.empty();
                const auto specified=arg(0)?utf8_to_utf16(text(0)):std::u16string{};
                if(has_tmp && exists(found->second.second))directory=found->second.second;
                else if(!specified.empty() && exists(specified))directory=specified;
                else if(has_tmp && !specified.empty())return error(2);
                else directory=runtime_.current_directory;
                if(!exists(directory))return error(2);
                prefix=arg(1)?utf8_to_utf16(text(1)):std::u16string{};
                if(prefix.find_first_of(u"\\/:*?\"<>|")!=prefix.npos)return error(22);
                directory=temp_absolute(full_windows_environment_path(std::move(directory),runtime_.current_directory,runtime_.environment));
            }else directory=temporary_directory();
            auto& sequence=allocated?runtime_.crt_tempnam_sequence:runtime_.crt_tmpnam_sequence;
            const std::uint64_t maximum=allocated?UINT64_MAX:secure?INT32_MAX:32767U;
            const auto base32=[](std::uint64_t value){
                constexpr char16_t digits[]=u"0123456789abcdefghijklmnopqrstuv";
                std::u16string text;do{text.push_back(digits[value&31U]);value>>=5;}while(value);
                std::reverse(text.begin(),text.end());return text;
            };
            for(unsigned attempts=0;attempts<32767U;++attempts){
                if(sequence>=maximum)return error(17);
                const auto ordinal=sequence++;
                auto name=directory;
                if(allocated)name+=prefix+utf8_to_utf16(std::to_string(ordinal+1));
                else name+=u"x64arm"+base32(runtime_.pointer_cookie&UINT32_MAX)+u"."+base32(ordinal);
                const auto info=filesystem_.information(utf16_to_utf8(name));
                if(!info.error)continue;
                if(info.error!=2 && info.error!=3)return error(info.error==5?13:22);
                const auto bytes=encoded(name);const auto count=bytes.size()/(wide?2U:1U);
                if(!allocated && (count>260 || (secure && arg(1)<count)))return error(34);
                std::uint64_t destination=arg(0);
                if(allocated){destination=allocate_heap(memory,bytes.size());if(!destination)return error(12);}
                else if(!destination){
                    auto& buffer=runtime_.crt_tmpnam_buffers[wide?1:0];
                    if(!buffer)buffer=runtime_allocate(memory,{},260*(wide?2U:1U));
                    destination=buffer;
                }
                try{memory.write(GuestAddress{destination},bytes);}
                catch(...){if(allocated)(void)free_heap(memory,destination,state);throw;}
                return finish(secure?0:destination);
            }
            return error(17);
        }catch(const std::invalid_argument&){return error(22);}
        catch(const std::bad_alloc&){return error(12);}
    }
    const auto namespace_state=filesystem_.volume_state();const auto known=filesystem_.volumes();
    const auto known_volume=[&](char id){return std::any_of(known.begin(),known.end(),[&](const auto& item){return item.id==id;});};
    const auto root_letter=[&](std::u16string_view path)->char{if(path.size()==49)return windows_volume_letter(path);if(path.starts_with(u"\\\\?\\") || path.starts_with(u"\\\\.\\"))path.remove_prefix(4);if(path.size()!=3 || path[1]!=u':' || (path[2]!=u'\\' && path[2]!=u'/'))return 0;auto letter=path[0];if(letter>=u'a' && letter<=u'z')letter=static_cast<char16_t>(letter-32);return letter>=u'A' && letter<=u'Z'?static_cast<char>(letter):0;};
    const auto output=[&](std::u16string_view value,std::uint64_t target,unsigned capacity,unsigned error){const auto bytes=encoded(value);if(bytes.size()/(wide?2U:1U)>capacity){fail(error);return false;}if(!target){fail(87);return false;}memory.write(GuestAddress{target},bytes);return true;};
    const auto mount_volume=[&](std::string path)->std::pair<char,std::uint32_t>{
        if(path.empty() || (path.back()!='\\' && path.back()!='/'))return {0,123};
        if(namespace_state.error)return {0,namespace_state.error};
        const auto root=root_letter(utf8_to_utf16(path));if(root){
            if(path.size()==49)return known_volume(root)?std::pair{root,0U}:std::pair{char{0},3U};
            const auto alias=namespace_state.drives.find(root);return alias!=namespace_state.drives.end()?std::pair{alias->second,0U}:std::pair{char{0},3U};
        }
        const auto point=filesystem_.reparse(std::move(path));if(point.error)return {0,point.error};return point.kind==3?std::pair{windows_volume_letter(std::string_view(point.target)),0U}:std::pair{char{0},4390U};
    };
    if(symbol.starts_with("SetVolumeMountPoint") || symbol.starts_with("DeleteVolumeMountPoint")){
        if(!arg(0) || (symbol.starts_with("Set") && !arg(1)))fail(87);
        else {const auto path=text(0);if(path.empty() || (path.back()!='\\' && path.back()!='/'))fail(123);
            else if(symbol.starts_with("Set")){const auto name=text(1);const auto target=name.size()==49?windows_volume_letter(std::string_view(name)):char{0};
                if(!target || !known_volume(target))fail(123);else {const auto code=filesystem_.set_volume_mount(path,target);if(code)fail(code);else result=1;}}
            else {const auto code=filesystem_.delete_volume_mount(path);if(code)fail(code);else result=1;}}
    }else if(symbol.starts_with("FindFirstVolumeMountPoint")){
        if(!arg(0))fail(87);else {const auto name=text(0);const auto id=name.size()==49?windows_volume_letter(std::string_view(name)):char{0};
            if(!id || !known_volume(id))fail(3);else {auto paths=filesystem_.volume_mount_paths(id,true);if(paths.error)fail(paths.error);else if(paths.paths.empty())fail(18);
                else if(runtime_.volume_mount_searches.size()>=256)fail(8);else {auto first=utf8_to_utf16(paths.paths.front());std::replace(first.begin(),first.end(),u'/',u'\\');
                    if(output(first,arg(1),u32(2),234)){result=runtime_.allocate_wait_handle();runtime_.volume_mount_searches.emplace(result,std::pair{std::move(paths.paths),std::size_t{1}});}}}}
        if(!result)result=x86_?UINT32_MAX:UINT64_MAX;
    }else if(symbol.starts_with("FindNextVolumeMountPoint")){
        const auto found=runtime_.volume_mount_searches.find(arg(0));if(found==runtime_.volume_mount_searches.end())fail(6);else {auto& [paths,position]=found->second;
            if(position>=paths.size())fail(18);else {auto path=utf8_to_utf16(paths[position]);std::replace(path.begin(),path.end(),u'/',u'\\');if(output(path,arg(1),u32(2),234)){++position;result=1;}}}
    }else if(symbol=="FindVolumeMountPointClose"){if(runtime_.volume_mount_searches.erase(arg(0)))result=1;else fail(6);}
    else if(symbol.starts_with("FindFirstVolume")){
        std::vector<char> letters;for(const auto& volume:known)letters.push_back(volume.id);
        if(namespace_state.error)fail(namespace_state.error);else if(letters.empty())fail(18);else if(runtime_.volume_searches.size()>=256)fail(8);else if(output(windows_volume_name(static_cast<char16_t>(letters.front())),arg(0),u32(1),234)){result=runtime_.allocate_wait_handle();runtime_.volume_searches.emplace(result,std::pair{std::move(letters),std::size_t{1}});}if(!result)result=x86_?UINT32_MAX:UINT64_MAX;
    }else if(symbol.starts_with("FindNextVolume")){
        const auto found=runtime_.volume_searches.find(arg(0));if(found==runtime_.volume_searches.end())fail(6);else {auto& [letters,position]=found->second;if(position>=letters.size())fail(18);else if(output(windows_volume_name(static_cast<char16_t>(letters[position])),arg(1),u32(2),234)){++position;result=1;}}
    }else if(symbol=="FindVolumeClose"){if(runtime_.volume_searches.erase(arg(0)))result=1;else fail(6);}
    else if(symbol.starts_with("GetVolumeNameForVolumeMountPoint")){
        if(!arg(0))fail(87);else {const auto [id,error]=mount_volume(text(0));if(error)fail(error);else if(output(windows_volume_name(static_cast<char16_t>(id)),arg(1),u32(2),206))result=1;}
    }else if(symbol.starts_with("GetVolumePathNamesForVolumeName")){
        if(!arg(0) || !arg(3))fail(87);else {const auto name=text(0);const auto id=name.size()==49?windows_volume_letter(std::string_view(name)):char{0};if(!id || !known_volume(id))fail(123);
            else {const auto paths=filesystem_.volume_mount_paths(id);if(paths.error)fail(paths.error);else {std::u16string value;for(const auto& path:paths.paths){auto p=utf8_to_utf16(path);std::replace(p.begin(),p.end(),u'/',u'\\');value+=p;value.push_back(0);}if(value.empty())value.push_back(0);
                const auto bytes=encoded(value);const auto count=bytes.size()/(wide?2U:1U);memory.store(GuestAddress{arg(3)},32,count);
                if(u32(2) && !arg(1))fail(87);else {if(u32(2))memory.write(GuestAddress{arg(1)},std::span(bytes).first(std::min<std::size_t>(count,u32(2))*(wide?2U:1U)));if(u32(2)<count)fail(234);else result=1;}}}}
    }else if(symbol.starts_with("GetVolumePathName")){
        if(!arg(0))fail(87);else {auto path=text(0);std::replace(path.begin(),path.end(),'\\','/');std::string root,prefix;std::size_t start=0;unsigned error=0;
            if(path.empty())error=0;
            else if(const auto id=windows_volume_letter(std::string_view(path))){root=windows_volume_path(id);std::replace(root.begin(),root.end(),'\\','/');start=49;}
            else {if(path.starts_with("//?/") || path.starts_with("//./")){prefix=path.substr(0,4);path.erase(0,4);}else if(path.starts_with("//"))error=53;
                if(path.size()>=2 && path[1]==':'){auto letter=path[0];if(letter>='a' && letter<='z')letter=static_cast<char>(letter-32);if(letter<'A' || letter>'Z')error=123;else {root=std::string(1,letter)+":/";start=2;}}
                else if(!prefix.empty())error=123;else root="C:/";}
            if(path.empty() || error)fail(error);else {
                // 未限定 volume 的相對／NT 路徑按 Windows 契約回 boot volume。
                std::string query=root;if(start){std::vector<std::string> parts;for(std::size_t at=start;at<path.size();){auto end=path.find('/',at);if(end==path.npos)end=path.size();auto part=path.substr(at,end-at);at=end+1;if(part==".."){if(!parts.empty())parts.pop_back();}else if(!part.empty() && part!=".")parts.push_back(std::move(part));}for(const auto& part:parts){if(query.back()!='/')query+='/';query+=part;}}
                auto location=filesystem_.volume_location(query,true);
                while(location.error && (location.error==2 || location.error==3 || location.error==5) && query.size()>root.size()){const auto end=query.find_last_of('/');query.resize(std::max(root.size(),end));location=filesystem_.volume_location(query,true);}
                if(location.error)fail(location.error==5?3U:location.error);else {auto value=utf8_to_utf16(prefix+location.mount_path);std::replace(value.begin(),value.end(),u'/',u'\\');if(u32(2)==value.size())value.pop_back();if(output(value,arg(1),u32(2),206))result=1;}
            }
        }
    }else
    if (symbol.starts_with("FindFirstChangeNotification")) {
        return invoke_directory_notification(state,memory,service);
    } else if (symbol == "FindNextChangeNotification" || symbol == "FindCloseChangeNotification" || symbol == "ReadDirectoryChangesW" || symbol == "ReadDirectoryChangesExW") {
        return invoke_directory_notification(state,memory,service);
    } else if(symbol.starts_with("QueryDosDevice")){
        std::u16string value;std::uint32_t error=0;
        if(namespace_state.error)error=namespace_state.error;
        else if(arg(0)){
            auto device=utf8_to_utf16(text(0));const auto volume=windows_volume_letter(std::u16string_view(u"\\\\?\\"+device+u"\\"));char id=0;
            if(volume){if(known_volume(volume))id=volume;else error=2;}
            else if(device.size()!=2 || device[1]!=u':')error=2;
            else {if(device[0]>=u'a' && device[0]<=u'z')device[0]=static_cast<char16_t>(device[0]-32);const auto letter=static_cast<char>(device[0]);
                const auto alias=namespace_state.drives.find(letter);if(alias==namespace_state.drives.end())error=2;else id=alias->second;}
            if(!error){value=windows_device_name(static_cast<char16_t>(id));value.push_back(0);}
        }else for(const auto& [letter,target]:namespace_state.drives){(void)target;value.push_back(static_cast<char16_t>(letter));value+=u":";value.push_back(0);}
        if(error)fail(error);
        else {const auto bytes=encoded(value);const auto count=bytes.size()/(wide?2U:1U);if(!arg(1) || u32(2)<count)fail(122);else {memory.write(GuestAddress{arg(1)},bytes);result=count;}}
    } else if (symbol == "GetLogicalDrives" || symbol.starts_with("GetLogicalDriveStrings")) {
        if(namespace_state.error){fail(namespace_state.error);arg.return_to(state,0);return std::nullopt;}
        std::uint32_t mask=0;std::u16string drives;
        for(char letter='A';letter<='Z';++letter){const auto path=std::string(1,letter)+":/";if(filesystem_.information(path).error)continue;mask|=1U<<(letter-'A');drives+=static_cast<char16_t>(letter);drives+=u":\\";drives.push_back(0);}
        if (symbol == "GetLogicalDrives") result = mask;
        else {
            const auto bytes = encoded(drives);
            const auto count = bytes.size() / (wide ? 2U : 1U);
            result = count;
            if (u32(0) >= count) { memory.write(GuestAddress{arg(1)}, bytes); result = count - 1; }
        }
    } else if (symbol.starts_with("GetDriveType")) {
        const auto path = arg(0) ? text(0) : std::string("C:\\");
        result = filesystem_.network_path(path)?4:!mount_volume(path).second?3:1;
    } else if (symbol.starts_with("GetDiskFreeSpace")) {
        const bool extended = symbol.starts_with("GetDiskFreeSpaceEx");
        const auto path = arg(0) ? text(0) : std::string("C:\\");
        if (!extended && !filesystem_.network_path(path) && mount_volume(path).second) fail(3);
        else {
            const auto space = filesystem_.volume_space(path);
            if (space.error) fail(space.error);
            else if (extended) {
                for (unsigned i = 1; i <= 3; ++i) if (arg(i)) memory.check(GuestAddress{arg(i)}, 8, Access::Write);
                if (arg(1)) memory.store(GuestAddress{arg(1)}, 64, space.available);
                if (arg(2)) memory.store(GuestAddress{arg(2)}, 64, space.total);
                if (arg(3)) memory.store(GuestAddress{arg(3)}, 64, space.free);
                result = 1;
            } else {
                // 舊式 DWORD cluster 介面不能表示超大磁碟時明確回 overflow，不截斷容量。
                if (space.total / space.allocation_unit > UINT32_MAX) fail(534);
                else if (!arg(1) || !arg(2) || !arg(3) || !arg(4)) fail(87);
                else {
                    for (unsigned i = 1; i <= 4; ++i) memory.check(GuestAddress{arg(i)}, 4, Access::Write);
                    memory.store(GuestAddress{arg(1)}, 32, space.allocation_unit / space.sector_size);
                    memory.store(GuestAddress{arg(2)}, 32, space.sector_size);
                    memory.store(GuestAddress{arg(3)}, 32, space.available / space.allocation_unit);
                    memory.store(GuestAddress{arg(4)}, 32, space.total / space.allocation_unit);
                    result = 1;
                }
            }
        }
    } else if (symbol.starts_with("SetVolumeLabel")) {
        if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許卷標宿主寫入");
        const auto path=arg(0)?text(0):utf16_to_utf8(runtime_.current_directory);
        const auto location=arg(0)?FileVolumeLocation{}:filesystem_.volume_location(path,true);
        const auto [id,root_error]=arg(0)?mount_volume(path):std::pair<char,std::uint32_t>{location.volume,location.error};
        const auto error=root_error?root_error:filesystem_.set_volume_label(id,arg(1)?(wide?read_utf16(memory,GuestAddress{arg(1)}):utf8_to_utf16(text(1))):std::u16string{});
        if(error)fail(error);else result=1;
    } else if (symbol.starts_with("GetVolumeInformation")) {
        const auto path = arg(0) ? text(0) : std::string("C:\\");
        const auto [id,root_error]=mount_volume(path);
        const auto info = filesystem_.information(root_error?path:windows_volume_path(id));
        const auto properties=root_error?FileVolumeProperties{}:filesystem_.volume_properties(id);
        const auto label=encoded(properties.label);
        if (root_error) fail(root_error==123 || root_error==4390?3U:root_error);
        else if (info.error) fail(info.error);
        else if(properties.error)fail(properties.error);
        else if ((arg(1) && u32(2) < label.size()/(wide?2U:1U)) || (arg(6) && u32(7) < 7)) fail(122);
        else {
            const auto name = encoded(u"X64ARM");
            if (arg(1)) memory.check(GuestAddress{arg(1)}, label.size(), Access::Write);
            if (arg(6)) memory.check(GuestAddress{arg(6)}, name.size(), Access::Write);
            for (unsigned i = 3; i <= 5; ++i) if (arg(i)) memory.check(GuestAddress{arg(i)}, 4, Access::Write);
            if (arg(1)) memory.write(GuestAddress{arg(1)}, label);
            if (arg(6)) memory.write(GuestAddress{arg(6)}, name);
            if (arg(3)) memory.store(GuestAddress{arg(3)}, 32, info.metadata.device);
            if (arg(4)) memory.store(GuestAddress{arg(4)}, 32, 255);
            if (arg(5)) memory.store(GuestAddress{arg(5)}, 32, 2 | 4 | (filesystem_.writable() ? 0 : 0x80000));
            result = 1;
        }
    } else if (symbol.starts_with("GetCompressedFileSize")) {
        if (!arg(0)) { fail(87); result = UINT32_MAX; }
        else {
            const auto info = filesystem_.target_information(text(0));
            if (info.error || (info.metadata.attributes & 0x10)) { fail(info.error ? info.error : 5); result = UINT32_MAX; }
            else {
                if (arg(1)) memory.store(GuestAddress{arg(1)}, 32, info.metadata.size >> 32);
                result = static_cast<std::uint32_t>(info.metadata.size); last_error_ = 0;
            }
        }
    } else if (symbol.starts_with("GetTempFileName")) {
        if(!arg(0) || !arg(3)){fail(87);arg.return_to(state,0);return std::nullopt;}
        auto path=utf8_to_utf16(text(0));auto prefix=arg(1)?utf8_to_utf16(text(1)):std::u16string{};prefix.resize(std::min<std::size_t>(3,prefix.size()));
        if(path.size()>246)fail(111);
        else if(path.empty() || prefix.find_first_of(u"\\/:*?\"<>|")!=prefix.npos)fail(123);
        else {
            const auto directory=filesystem_.information(utf16_to_utf8(path));
            if(directory.error || !(directory.metadata.attributes&0x10)){fail(267);arg.return_to(state,0);return std::nullopt;}
            if(path.back()!=u'\\' && path.back()!=u'/')path+=u'\\';
            memory.check(GuestAddress{arg(3)},260*(wide?2U:1U),Access::Write);
            const bool create=u32(2)==0;auto unique=static_cast<std::uint16_t>(create ? runtime_.clock_ticks : u32(2));
            if(!unique)unique=1;
            for(unsigned attempts=0;attempts<65535;++attempts){
                constexpr char16_t digits[]=u"0123456789ABCDEF";
                auto filename=path+prefix;for(int shift=12;shift>=0;shift-=4)filename+=digits[(unique>>shift)&15];filename+=u".tmp";
                if(create){
                    auto opened=filesystem_.open_host(utf16_to_utf8(filename),0x40000000U,1,0,7);
                    if(opened.error==80 || opened.error==183 || opened.error==32){if(!++unique)++unique;if(attempts==65534)fail(80);continue;}
                    if(opened.error){fail(opened.error);break;}
                }
                memory.write(GuestAddress{arg(3)},encoded(filename));result=unique;break;
            }
            if(!result && !last_error_)last_error_=80;
        }
    } else if (symbol.starts_with("GetTempPath")) {
        try {
            const auto path = temporary_directory();
            if (path.size() > 260) fail(206);
            else {
                const auto bytes = encoded(path); const auto count = bytes.size() / (wide ? 2U : 1U);
                result = count;
                if (u32(0) >= count) { memory.write(GuestAddress{arg(1)}, bytes); result = count - 1; }
            }
        } catch (const std::invalid_argument&) { fail(123); }
    } else throw HostCallFault("未知的 Windows 磁碟區查詢服務");
    arg.return_to(state, result); return std::nullopt;
}
}
