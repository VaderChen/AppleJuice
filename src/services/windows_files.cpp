#include "x64arm/windows_services.hpp"
#include "crt_os_error.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_security_attributes.hpp"
#include "x64arm/security_access.hpp"
#include "x64arm/windows_paths.hpp"
#include "x64arm/windows_raw_input.hpp"
#include "x64arm/shared_wake.hpp"
#include <algorithm>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <system_error>
#include <thread>
#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace x64arm {
namespace {
bool eager_file_information() {
    static const bool legacy=[] {const auto* value=std::getenv("X64ARM_FILE_OPEN_LEGACY_INFORMATION");
        return value && std::string_view(value)=="1";}();
    return legacy;
}
// 僅在指定的檔案服務診斷輪量測；不記錄檔名，不在客體迴圈增加輪詢。
struct FileOpenCpuProfile {
    bool enabled{},valid{true},information_queried{};
    std::uint64_t begin{},information_cpu{},host_open_cpu{};
    FileOpenCpuProfile() {
        static const bool requested=[] {const auto* value=std::getenv("X64ARM_FILE_OPEN_PROFILE");
            return value && std::string_view(value)=="1";}();
        enabled=requested;begin=now();
    }
    std::uint64_t now() {
        if(!enabled)return 0;
#if defined(CLOCK_THREAD_CPUTIME_ID) && !defined(_WIN32)
        const auto saved=errno;timespec stamp{};
        const auto result=clock_gettime(CLOCK_THREAD_CPUTIME_ID,&stamp);errno=saved;
        if(result){valid=false;return 0;}
        return static_cast<std::uint64_t>(stamp.tv_sec)*1000000000ULL+static_cast<std::uint64_t>(stamp.tv_nsec);
#else
        valid=false;return 0;
#endif
    }
    void add(std::uint64_t& value,std::uint64_t start) {
        if(!enabled)return;
        const auto end=now();if(end<start){valid=false;return;}value+=end-start;
    }
    ~FileOpenCpuProfile() {
        if(!enabled)return;
        const auto end=now();if(!valid || end<begin || information_cpu+host_open_cpu>end-begin)return;
#if !defined(_WIN32)
        const auto saved=errno;
        std::fprintf(stderr,"file_open_cpu pid=%d unix_ms=%lld mode=%s information=%u information_cpu_ns=%llu host_open_cpu_ns=%llu other_cpu_ns=%llu total_cpu_ns=%llu\n",
            static_cast<int>(::getpid()),static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()),
            eager_file_information()?"eager":"demand",information_queried?1U:0U,
            static_cast<unsigned long long>(information_cpu),static_cast<unsigned long long>(host_open_cpu),
            static_cast<unsigned long long>(end-begin-information_cpu-host_open_cpu),static_cast<unsigned long long>(end-begin));
        errno=saved;
#endif
    }
};
bool find_matches(std::u16string pattern,std::u16string_view name,bool case_sensitive) {
    if(pattern==u"*.*")pattern=u"*";
    std::u16string expression;
    for(std::size_t i=0;i<pattern.size();++i) {
        const auto c=pattern[i];
        if(c==u'?' )expression.push_back(u'>');
        else if(c==u'.' && i+1==pattern.size() && i && pattern[i-1]==u'*')expression.back()=u'<';
        else expression.push_back(c==u'.' && i+1<pattern.size() && (pattern[i+1]==u'*' || pattern[i+1]==u'?')?u'"':c);
    }
    const auto fold=[&](char16_t c){return !case_sensitive && c>=u'A' && c<=u'Z'?static_cast<char16_t>(c+32):c;};
    std::vector<bool> previous(name.size()+1),next(name.size()+1);previous[0]=true;
    const auto last_dot=name.find_last_of(u'.');
    for(const auto c:expression) {
        std::fill(next.begin(),next.end(),false);
        for(std::size_t j=0;j<=name.size();++j) {
            if(c==u'*' || c==u'<')next[j]=previous[j] || (j && next[j-1] && (c==u'*' || j-1!=last_dot));
            else if(c==u'>')next[j]=(previous[j] && (j==name.size() || name[j]==u'.')) || (j && previous[j-1] && name[j-1]!=u'.');
            else if(c==u'"')next[j]=(j==name.size() && previous[j]) || (j && previous[j-1] && name[j-1]==u'.');
            else next[j]=j && previous[j-1] && fold(c)==fold(name[j-1]);
        }
        previous.swap(next);
    }
    return previous.back();
}
// Win32 與 CRT 使用同一份目錄快照及 DOS 萬用字元規則，僅輸出 ABI 不同。
std::pair<std::uint32_t,DirectorySearch> find_directory(ReadOnlyFiles& files,std::string path,bool case_sensitive=false,bool directories_only=false) {
    std::replace(path.begin(),path.end(),'\\','/');const auto split=path.find_last_of('/');
    const auto parent=split==path.npos?std::string("."):path.substr(0,split+1),pattern=split==path.npos?path:path.substr(split+1);
    if(pattern.empty())return {2,{}};
    if(pattern.size()>1040)return {8,{}};
    const auto expression=utf8_to_utf16(pattern);
    // 只為可能符合的名稱查詢中繼資料；不分大小寫的比對涵蓋區分大小寫的結果與同名補入項目。
    const auto directory=files.list(parent,[&](std::string_view name){return find_matches(expression,utf8_to_utf16(name),false);});
    if(directory.error)return {directory.error,{}};
    DirectorySearch search;
    for(const auto& entry:directory.entries)
        if((!directories_only || (entry.metadata.attributes&0x10)) && find_matches(expression,utf8_to_utf16(entry.name),case_sensitive))search.entries.push_back(entry);
    const auto error=search.entries.empty()?2U:0U;return {error,std::move(search)};
}
bool crt_find_data(GuestMemory& memory,std::uint64_t output,const DirectoryEntry& entry,std::string_view symbol) {
    const bool wide=symbol.starts_with("_w"),time64=symbol.ends_with("64i32") || (symbol.ends_with("64") && !symbol.ends_with("32i64"));
    const bool size64=symbol.ends_with("64");const unsigned time_start=time64?8U:4U,time_unit=time64?8U:4U;
    const unsigned size_start=time_start+3*time_unit,name_start=size_start+(size64?8U:4U),alignment=time64 || size64?8U:4U;
    const unsigned extent=(name_start+(wide?520U:260U)+alignment-1)/alignment*alignment;
    auto name=wide?utf16_bytes(utf8_to_utf16(entry.name)):std::vector<std::uint8_t>(entry.name.begin(),entry.name.end());
    if(!wide)name.push_back(0);if(name.size()>(wide?520U:260U))return false;
    std::vector<std::uint8_t> bytes(extent);const auto put=[&](unsigned at,std::uint64_t value,unsigned count){for(unsigned i=0;i<count;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
    const auto& info=entry.metadata;put(0,info.attributes&0x37U,4);put(size_start,info.size,size64?8U:4U);
    const std::array times{info.created,info.accessed,info.modified};
    for(unsigned i=0;i<times.size();++i){
        const auto seconds=(static_cast<__int128>(times[i])-UINT64_C(116444736000000000))/10000000;
        const auto maximum=time64?INT64_C(32535215999):INT64_C(2147483647);
        put(time_start+i*time_unit,seconds<0 || seconds>maximum?UINT64_MAX:static_cast<std::uint64_t>(seconds),time_unit);
    }
    std::copy(name.begin(),name.end(),bytes.begin()+name_start);memory.write(GuestAddress{output},bytes);return true;
}
void stream_data(GuestMemory& memory,std::uint64_t address,const DirectoryEntry& entry) {
    constexpr std::size_t capacity=600; // LARGE_INTEGER + WCHAR[MAX_PATH + 36]
    const auto name=utf16_bytes(utf8_to_utf16(entry.name));
    if(name.size()>capacity-8)throw HostCallFault("串流名稱超過 WIN32_FIND_STREAM_DATA 容量");
    std::array<std::uint8_t,capacity> bytes{};
    for(unsigned n=0;n<8;++n)bytes[n]=static_cast<std::uint8_t>(entry.metadata.size>>(n*8));
    std::copy(name.begin(),name.end(),bytes.begin()+8);memory.write(GuestAddress{address},bytes);
}
void find_data(GuestMemory& memory,std::uint64_t address,const DirectoryEntry& entry,bool wide) {
    auto name=wide?utf16_bytes(utf8_to_utf16(entry.name)):std::vector<std::uint8_t>(entry.name.begin(),entry.name.end());
    if(!wide)name.push_back(0);
    if(name.size()>(wide?520U:260U))throw HostCallFault("FindFirstFile 名稱超過 WIN32_FIND_DATA 容量");
    memory.write(GuestAddress{address},std::vector<std::uint8_t>(wide?592U:320U));
    const auto& info=entry.metadata;memory.store(GuestAddress{address},32,info.attributes);
    memory.store(GuestAddress{address+4},64,info.created);memory.store(GuestAddress{address+12},64,info.accessed);memory.store(GuestAddress{address+20},64,info.modified);
    memory.store(GuestAddress{address+28},32,info.size>>32);memory.store(GuestAddress{address+32},32,info.size);memory.write(GuestAddress{address+44},name);
    if(info.attributes&0x400U)memory.store(GuestAddress{address+36},32,info.reparse_tag?info.reparse_tag:0xa000000cU);
}
}
std::optional<std::uint32_t> WindowsServices::invoke_file_open(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    FileOpenCpuProfile profile;
    const WindowsCall frame(state,memory,service);
    FileOpenArguments values{};
    const auto invalid=[&](std::uint32_t error){last_error_=error;frame.return_to(state,x86_?UINT32_MAX:UINT64_MAX);return std::optional<std::uint32_t>{};};
    std::u16string guest_path;
    try {
        if(!frame(0))return invalid(87);
        guest_path=service.symbol=="CreateFileA"?utf8_to_utf16(read_ansi(memory,frame(0))):read_utf16(memory,GuestAddress{frame(0)});
        if(service.symbol=="CreateFile2") {
            values={frame(0),frame(1),frame(2),0,frame(3),0x80,0};
            if(const auto extended=frame(4)) {
                const auto extent=x86_?24U:32U;memory.check(GuestAddress{extended},extent,Access::Read);
                if(memory.load(GuestAddress{extended},32)!=extent)return invalid(87);
                const auto attributes=static_cast<std::uint32_t>(memory.load(GuestAddress{extended+4},32));
                const auto flags=static_cast<std::uint32_t>(memory.load(GuestAddress{extended+8},32));
                const auto qos=static_cast<std::uint32_t>(memory.load(GuestAddress{extended+12},32));
                if((attributes&0xffff0000U) || (flags&~0xffb60000U) || (qos&~0x001f0000U))return invalid(87);
                values[3]=memory.load(GuestAddress{extended+16},x86_?32:64);
                values[5]=attributes|flags|qos;
                values[6]=memory.load(GuestAddress{extended+(x86_?20:24)},x86_?32:64);
            }
        } else for(unsigned i=0;i<values.size();++i)values[i]=frame(i);
    }catch(const MemoryFault&){return invalid(87);}catch(const std::invalid_argument&){return invalid(123);}catch(const std::bad_alloc&){return invalid(8);}
    auto canonical=service;if(service.symbol=="CreateFile2")canonical.symbol="CreateFileW";
    if(bluetooth_path(guest_path))return open_bluetooth(state,memory,canonical,guest_path,values);
    if(hid_path(guest_path))return open_hid(state,memory,canonical,guest_path,&values);
    if(usb::interface_path(guest_path))return open_usb(state,memory,canonical,guest_path,&values);
    if(serial_port(guest_path))return invoke_serial(state,memory,canonical,&values);
    if(kernel_device_path(guest_path))return open_kernel_device(state,memory,canonical,guest_path,values);
    if(guest_path.starts_with(u"\\\\.\\")){
        // \\.\ 是 DOS 裝置命名空間：管道、磁碟區、實體磁碟與 mailslot 走既有流程；其餘名稱不存在時如同 Windows 回報找不到檔案。
        std::u16string device(guest_path.substr(4));for(auto& c:device)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);
        const bool known=device.starts_with(u"pipe\\") || device.starts_with(u"mailslot\\") || device.starts_with(u"physicaldrive") ||
            (device.size()==2 && device[1]==u':') || device.starts_with(u"globalroot\\");
        if(!known){last_error_=2;WindowsCall(state,memory,canonical).return_to(state,x86_?UINT32_MAX:UINT64_MAX);return std::nullopt;}
        return invoke_file_io(state,memory,canonical,&values);
    }
    const auto arg=[&](unsigned i){return values.at(i);};
    const auto dword=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    std::uint64_t returned=UINT64_MAX;
    if(filesystem_.writable() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許開啟可寫宿主檔案");
    CreationSecurity security;std::uint32_t security_error=0;
    try{security=read_security_attributes(memory,arg(3),x86_,false);}catch(const SecurityDescriptorError& error){security_error=error.code;}catch(const MemoryFault&){security_error=87;}
    // FILE_FLAG_NO_BUFFERING（0x20000000）只要求呼叫端以磁區對齊讀寫並略過系統快取，不改變資料內容；宿主照常讀寫。
    // Steam 驗證下載暫存檔以此旗標開檔，拒絕時整個更新被判為「磁碟讀取錯誤」。
    const std::uint32_t read_access=filesystem_.writable()?0xc01f01bfU:0x801200a9U;constexpr std::uint32_t supported_flags=0xfe3f01a7U;
    if ((dword(1)&~read_access) || (!filesystem_.writable() && (dword(4)!=3 || (dword(5)&0x04000000U))) || (dword(2)&~7U) || (dword(5)&~supported_flags)) last_error_=50;
    else if (security_error) last_error_=security_error;
    else if (files_.size()>=1024) last_error_=4;
    else {
        std::string path;
        try {path=utf16_to_utf8(guest_path);} catch (const std::invalid_argument&) {last_error_=123;}
        if (path.empty()) last_error_=123;
        else {
            refresh_file_security_context();const bool manage_volume_at_open=runtime_.token_objects.at(effective_token_id()).has_privilege(28,true);
            const auto install=[&](FileHandle file) {
                file.manage_volume_at_open=manage_volume_at_open;
                const auto handle=runtime_.allocate_wait_handle();file.object=handle;
                try {runtime_.win_file_positions.emplace(handle,0);files_.emplace(handle,std::move(file));if(security.inherit)runtime_.handle_flags[handle]=1;returned=handle;return true;}
                catch(const std::bad_alloc&) {files_.erase(handle);runtime_.win_file_positions.erase(handle);last_error_=8;returned=UINT64_MAX;return false;}
            };
            FileInformation info;bool information_loaded=false;
            const auto load_information=[&] {
                if(information_loaded)return;
                const auto started=profile.now();profile.information_queried=true;
                info=filesystem_.information(path);profile.add(profile.information_cpu,started);information_loaded=true;
            };
            // 可寫掛載由 open_host 保留路徑、權限、inode 分享與建立契約。
            // 只有相容性可能衝突的非 native 快照需要先解析 key；唯讀快照仍使用完整中繼資料。
            if(eager_file_information() || !filesystem_.writable())load_information();
            const auto sharing_conflict=[&](const auto& entry) {
                if(entry.second.native)return false;
                const auto category=[](std::uint32_t access){return ((access&(0x80000000U|1U|0x20U))?1U:0U)|((access&(0x40000000U|2U|4U))?2U:0U)|((access&0x10000U)?4U:0U);};
                if(!(category(dword(1))&~entry.second.sharing) && !(category(entry.second.access)&~dword(2)))return false;
                load_information();return entry.second.key==info.key;
            };
            if (std::any_of(files_.begin(),files_.end(),sharing_conflict)) last_error_=32;
            else if (filesystem_.writable()) {
                const auto file_template=[&]() -> OpenHostFile {
                    if(!arg(6))return {};
                    const auto found=files_.find(arg(6));if(found==files_.end())return {{},{},{},6,false};
                    const auto& original=found->second;
                    if(!(original.access&(0x80000000U|1U)))return {{},{},{},5,false};
                    const auto file=original.native?original.native:original.identity;
                    if(!file)return {{},{},{},6,false};const auto details=file->information();
                    if(details.error)return {details.key,{},{},details.error,false};
                    if(details.metadata.attributes&0x10U)return {{},{},{},5,false};
                    return {details.key,file,details.metadata,0,true};
                };
                const auto opening=profile.now();
                const auto opened=filesystem_.open_host(path,dword(1),dword(4),dword(5),dword(2),[&]{return file_creation_descriptor(memory,arg(3));},arg(6)?std::function<OpenHostFile()>{file_template}:std::function<OpenHostFile()>{});
                profile.add(profile.host_open_cpu,opening);
                if(opened.error)last_error_=opened.error;
                else {
                    FileHandle file;file.key=opened.key;file.bytes=std::make_shared<const std::vector<std::uint8_t>>();file.native=opened.file;file.metadata=opened.metadata;
                    file.sharing=dword(2);file.access=dword(1)|((dword(5)&0x04000000U)?0x10000U:0U);file.mode=(dword(5)&0x40000000U)?0U:0x20U;file.path=path;
                    if(install(std::move(file)) && (dword(4)==2 || dword(4)==4))last_error_=opened.existed?183:0;
                }
            } else if (info.error) last_error_=info.error;
            else if ((info.metadata.attributes&0x10) && !(dword(5)&0x02000000U)) last_error_=5;
            else if (const auto error=filesystem_.security_access(path,dword(1)))last_error_=error;
            else {
                {
                    const auto snapshot=(info.metadata.attributes&0x10)?FileSnapshot{info.key,std::make_shared<const std::vector<std::uint8_t>>(),0}:filesystem_.open(path);
                    if (snapshot.error) last_error_=snapshot.error;
                    else {
                        FileHandle file;file.key=info.key;file.bytes=snapshot.bytes;file.sharing=dword(2);file.access=dword(1);
                        file.mode=(dword(5)&0x40000000U)?0U:0x20U;file.metadata=info.metadata;file.path=path;
                        const auto identity=filesystem_.pin(path);if(identity.error)last_error_=identity.error;else {file.identity=identity.file;(void)install(std::move(file));}
                    }
                }
            }
        }
    }
    frame.return_to(state,returned);return std::nullopt;
}
std::span<const WindowsServiceDescriptor> file_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"ntdll.dll","NtQueryInformationFile",5},{"ntdll.dll","NtSetInformationFile",5},
        {"ntdll.dll","NtQueryVolumeInformationFile",5},{"ntdll.dll","NtQueryDirectoryFile",11},
        {"ntdll.dll","NtQueryDirectoryFileEx",10},
        {"ntdll.dll","NtDeviceIoControlFile",10},
        {"kernel32.dll","GetFinalPathNameByHandleA",4},{"kernel32.dll","GetFinalPathNameByHandleW",4},{"kernel32.dll","ReOpenFile",4},{"kernel32.dll","SetFileInformationByHandle",4},
        {"kernel32.dll","SetFileValidData",2,2},
        {"kernel32.dll","GetBinaryTypeA",2},{"kernel32.dll","GetBinaryTypeW",2},
        {"kernel32.dll","CreateDirectoryA",2},{"kernel32.dll","CreateDirectoryW",2},
        {"kernel32.dll","CreateDirectoryExA",3},{"kernel32.dll","CreateDirectoryExW",3},
        {"kernel32.dll","RemoveDirectoryA",1},{"kernel32.dll","RemoveDirectoryW",1},
        {"kernel32.dll","DeleteFileA",1},{"kernel32.dll","DeleteFileW",1},
        {"kernel32.dll","CopyFileExA",6},{"kernel32.dll","CopyFileExW",6},{"kernel32.dll","CopyFile2",3},
        {"kernel32.dll","CopyFileA",3},{"kernel32.dll","CopyFileW",3},
        {"kernel32.dll","CreateHardLinkA",3},{"kernel32.dll","CreateHardLinkW",3},
        {"kernel32.dll","CreateSymbolicLinkA",3},{"kernel32.dll","CreateSymbolicLinkW",3},
        {"shlwapi.dll","PathFileExistsA",1},{"shlwapi.dll","PathFileExistsW",1},
        {"kernel32.dll","ReplaceFileA",6},{"kernel32.dll","ReplaceFileW",6},
        {"kernel32.dll","MoveFileWithProgressA",5},{"kernel32.dll","MoveFileWithProgressW",5},
        {"kernel32.dll","MoveFileA",2},{"kernel32.dll","MoveFileW",2},{"kernel32.dll","MoveFileExA",3},{"kernel32.dll","MoveFileExW",3},
        {"kernel32.dll","SetEndOfFile",1},{"kernel32.dll","FlushFileBuffers",1},{"kernel32.dll","GetFileSize",2},{"kernel32.dll","SetFilePointer",4},
        {"kernel32.dll","DeviceIoControl",8},{"kernel32.dll","GetOverlappedResult",4},{"kernel32.dll","GetOverlappedResultEx",5},
        {"kernel32.dll","SetFileTime",4},{"kernel32.dll","GetFileTime",4},
        {"kernel32.dll","FindFirstFileA",2},{"kernel32.dll","FindFirstFileW",2},{"kernel32.dll","FindFirstFileExA",6},{"kernel32.dll","FindFirstFileExW",6},
        {"kernel32.dll","FindFirstStreamW",4},{"kernel32.dll","FindNextStreamW",2},
        {"kernel32.dll","FindNextFileA",2},{"kernel32.dll","FindNextFileW",2},{"kernel32.dll","FindClose",1},
        {"crt","_mkdir",1},{"crt","_wmkdir",1},{"crt","_rmdir",1},{"crt","_wrmdir",1},
        {"crt","_access",2},{"crt","_waccess",2},{"crt","_access_s",2},{"crt","_waccess_s",2},{"crt","_chmod",2},{"crt","_wchmod",2},
        {"crt","_findclose",1},
        {"crt","_findfirst32",2},{"crt","_findfirst32i64",2},{"crt","_findfirst64i32",2},{"crt","_findfirst64",2},
        {"crt","_findnext32",2},{"crt","_findnext32i64",2},{"crt","_findnext64i32",2},{"crt","_findnext64",2},
        {"crt","_wfindfirst32",2},{"crt","_wfindfirst32i64",2},{"crt","_wfindfirst64i32",2},{"crt","_wfindfirst64",2},
        {"crt","_wfindnext32",2},{"crt","_wfindnext32i64",2},{"crt","_wfindnext64i32",2},{"crt","_wfindnext64",2},
        {"crt","remove",1},{"crt","_wremove",1},{"crt","_unlink",1},{"crt","_wunlink",1},{"crt","rename",2},{"crt","_wrename",2},
        {"kernel32.dll","GetFileAttributesA",1},{"kernel32.dll","GetFileAttributesW",1},
        {"kernel32.dll","SetFileAttributesA",2},{"kernel32.dll","SetFileAttributesW",2},
        {"kernel32.dll","GetFileAttributesExA",3},{"kernel32.dll","GetFileAttributesExW",3},
        {"kernel32.dll","GetFileInformationByHandle",2},{"kernel32.dll","GetFileInformationByHandleEx",4},
        {"kernel32.dll","GetVolumeInformationByHandleW",8},
        {"crt","_stat32",2},{"crt","_stat32i64",2},{"crt","_stat64i32",2},{"crt","_stat64",2},
        {"crt","_fstat32",2},{"crt","_fstat32i64",2},{"crt","_fstat64i32",2},{"crt","_fstat64",2},{"crt","_fstat",2},{"crt","_fstati64",2},
        {"crt","_wstat32",2},{"crt","_wstat32i64",2},{"crt","_wstat64i32",2},{"crt","_wstat64",2},
        {"crt","_stat",2},{"crt","_stati64",2},{"crt","_wstat",2},{"crt","_wstati64",2}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_file(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    if(service.symbol=="GetOverlappedResult" || service.symbol=="GetOverlappedResultEx")return invoke_overlapped_result(state,memory,service);
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;
    if(kernel_device_handle(arg(0)))return invoke_kernel_device_io(state,memory,service);
    if(const auto file=files_.find(arg(0));file!=files_.end() && (file->second.serial || file->second.hid || file->second.usb)){
        if(symbol.starts_with("Nt")){
            const auto iosb=arg(symbol=="NtDeviceIoControlFile" || symbol=="NtQueryDirectoryFile" || symbol=="NtQueryDirectoryFileEx"?4:1);const auto unit=x86_?4U:8U;
            memory.check(GuestAddress{iosb},2*unit,Access::Write);memory.store(GuestAddress{iosb},static_cast<std::uint8_t>(unit*8),0xc0000010);memory.store(GuestAddress{iosb+unit},static_cast<std::uint8_t>(unit*8),0);
            arg.return_to(state,0xc0000010);return std::nullopt;
        }
        if(symbol=="GetFileInformationByHandle" || symbol=="GetFileInformationByHandleEx" || symbol=="GetFileTime" || symbol=="SetFileTime"){last_error_=1;arg.return_to(state,0);return std::nullopt;}
    }
    if(symbol.starts_with("GetFinalPathNameByHandle") || symbol=="ReOpenFile" || symbol=="SetFileInformationByHandle" || symbol=="SetFileValidData")return invoke_file_control(state,memory,service);
    const auto u32=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    std::uint64_t result=0;
    const auto fail=[&](std::uint32_t code){last_error_=code;result=0;};
    const auto name_information=[&](const FileHandle& file) {
        std::u16string name;std::uint32_t error=0;
        if(file.pipe)name=file.pipe->file_name();
        else {
            const auto source=file.native?file.native:file.identity;
            auto path=file.path.empty()?file.key:file.path;
            if(source){const auto current=filesystem_.final_path(*source,path);error=current.error;path=current.key;}
            if(!error){name=utf8_to_utf16(path);std::replace(name.begin(),name.end(),u'/',u'\\');
                if(name.size()>=2 && name[1]==u':')name.erase(0,2);
                if(name.empty() || name.front()!=u'\\')error=3;}
        }
        auto bytes=utf16_bytes(name);bytes.resize(bytes.size()-2);const auto length=bytes.size();
        bytes.insert(bytes.begin(),4,0);for(unsigned n=0;n<4;++n)bytes[n]=static_cast<std::uint8_t>(length>>(8*n));
        return std::pair{std::move(bytes),error};
    };
    const auto attributes=[&](std::uint64_t address,const FileMetadata& info) {
        memory.write(GuestAddress{address},std::array<std::uint8_t,36>{});
        memory.store(GuestAddress{address},32,info.attributes);
        memory.store(GuestAddress{address+4},64,info.created);memory.store(GuestAddress{address+12},64,info.accessed);memory.store(GuestAddress{address+20},64,info.modified);
        memory.store(GuestAddress{address+28},32,info.size>>32);memory.store(GuestAddress{address+32},32,info.size);
    };
    const bool crt_find=service.family=="crt" && (symbol.starts_with("_find") || symbol.starts_with("_wfind"));
    if(crt_find || symbol=="_access" || symbol=="_waccess" || symbol=="_access_s" || symbol=="_waccess_s" || symbol=="_chmod" || symbol=="_wchmod"){
        const bool secure=symbol.ends_with("_s"),first=symbol.find("findfirst")!=symbol.npos,close=symbol=="_findclose";
        const auto invalid=[&]()->std::optional<std::uint32_t>{
            memory.store(GuestAddress{runtime_.errno_cell},32,22);
            if(current_invalid_parameter_handler()){start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,secure?22:UINT64_MAX);return std::nullopt;}
            return 0xc0000417U;
        };
        const auto error=[&](unsigned value){memory.store(GuestAddress{runtime_.errno_cell},32,value);result=secure?value:UINT64_MAX;};
        if(crt_find){
            if(!close && (!arg(1) || (first && !arg(0))))return invalid();
            if(first){
                const auto path=symbol.starts_with("_w")?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)})):read_ansi(memory,arg(0));
                if((symbol.starts_with("_w")?utf8_to_utf16(path).size():path.size())>=260)error(22);
                else if(runtime_.directory_searches.size()>=1024)error(12);
                else{
                    auto [status,search]=find_directory(filesystem_,path);
                    if(status)error(status==2 || status==3?2:status==8?12:22);
                    else if(!crt_find_data(memory,arg(1),search.entries[0],symbol))error(22);
                    else{search.cursor=1;result=runtime_.allocate_wait_handle();runtime_.directory_searches.emplace(result,std::move(search));}
                }
            }else{
                const auto found=runtime_.directory_searches.find(arg(0));
                if(found==runtime_.directory_searches.end() || found->second.streams)error(22);
                else if(close)runtime_.directory_searches.erase(found);
                else if(found->second.cursor==found->second.entries.size())error(2);
                else if(!crt_find_data(memory,arg(1),found->second.entries[found->second.cursor],symbol))error(12);
                else ++found->second.cursor;
            }
        }else{
            const bool chmod=symbol=="_chmod" || symbol=="_wchmod";
            if(!arg(0) || (!chmod && (u32(1)&~6U)))return invalid();
            const auto path=symbol.starts_with("_w")?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)})):read_ansi(memory,arg(0));
            const auto file=filesystem_.information(path);auto status=file.error;
            if(!status && chmod){
                if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許 CRT 修改宿主檔案屬性");
                const auto flags=(file.metadata.attributes&~1U)|((u32(1)&0x80U)?0U:1U);status=filesystem_.set_attributes(path,flags);
            }else if(!status && (u32(1)&2U) && (file.metadata.attributes&1U) && !(file.metadata.attributes&16U))status=5;
            if(status){crt_os_error::store(memory,runtime_.errno_cell,status);result=secure?memory.load(GuestAddress{runtime_.errno_cell},32):UINT64_MAX;}
        }
        arg.return_to(state,result);return std::nullopt;
    }
    if(symbol=="GetBinaryTypeA" || symbol=="GetBinaryTypeW"){
        if(!arg(0) || !arg(1))fail(87);
        else {
            const auto path=symbol.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)})):read_ansi(memory,arg(0));
            const auto file=filesystem_.open(path);
            if(file.error)fail(file.error);
            else {
                const auto& bytes=*file.bytes;
                const auto fits=[&](std::size_t at,std::size_t size){return at<=bytes.size() && size<=bytes.size()-at;};
                const auto get=[&](std::size_t at,unsigned size){std::uint32_t value=0;for(unsigned i=0;i<size;++i)value|=static_cast<std::uint32_t>(bytes[at+i])<<(i*8);return value;};
                std::optional<std::uint32_t> type;
                if(fits(0,64) && get(0,2)==0x5a4d){
                    const auto offset=static_cast<std::size_t>(get(60,4));
                    if(offset>=64 && fits(offset,4) && get(offset,4)==0x4550){
                        if(fits(offset,24)){
                            const auto flags=get(offset+22,2),optional_size=get(offset+20,2);
                            if((flags&2U) && !(flags&0x2000U) && optional_size>=70 && fits(offset+24,optional_size)){
                                const auto magic=get(offset+24,2),subsystem=get(offset+24+68,2);
                                if(magic==0x10b)type=subsystem==7?4U:0U;
                                else if(magic==0x20b)type=6;
                            }
                        }
                    }else if(offset>=64 && fits(offset,2) && get(offset,2)==0x454e){
                        if(fits(offset,64) && !(get(offset+12,2)&0x8000U)){
                            const auto target=get(offset+54,1);if(target==2 || target==4)type=2;else if(target==1)type=5;
                        }
                    }else if(offset>=64 && fits(offset,2) && (get(offset,2)==0x454c || get(offset,2)==0x584c)){
                        // LE／LX 不宣稱為 Windows 可執行格式。
                    }else type=1;
                }else {
                    const auto dot=path.find_last_of('.');auto extension=dot==path.npos?std::string{}:path.substr(dot);
                    for(auto& c:extension)if(c>='A' && c<='Z')c=static_cast<char>(c+32);
                    if(!bytes.empty() && extension==".com")type=1;else if(!bytes.empty() && extension==".pif")type=3;
                }
                if(type){memory.store(GuestAddress{arg(1)},32,*type);result=1;}else fail(193);
            }
        }
        arg.return_to(state,result);return std::nullopt;
    }
    if(symbol=="NtDeviceIoControlFile" || symbol=="NtSetInformationFile" || symbol=="NtQueryVolumeInformationFile" || symbol=="NtQueryDirectoryFile" || symbol=="NtQueryDirectoryFileEx") {
        const bool control=symbol=="NtDeviceIoControlFile",extended=symbol=="NtQueryDirectoryFileEx",directory=extended || symbol=="NtQueryDirectoryFile";
        const auto iosb=arg(control || directory?4:1);const unsigned width=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(width*8);
        memory.check(GuestAddress{iosb},width*2,Access::Write);
        auto found=files_.find(arg(0));std::uint32_t status=0;std::uint64_t transferred=0;
        const auto nt_error=[](std::uint32_t error)->std::uint32_t {
            switch(error){case 0:return 0;case 2:case 3:return 0xc0000034;case 5:return 0xc0000022;case 6:return 0xc0000008;case 8:return 0xc000009a;case 32:return 0xc0000043;case 87:return 0xc000000d;case 112:return 0xc000007f;default:return 0xc00000bb;}
        };
        if(found==files_.end())status=0xc0000008;
        else if(control && (arg(2) || found->second.completion_port))status=0xc00000bb;
        else if(directory && arg(2) && found->second.completion_port)status=0xc000000d;
        else if(directory && (arg(1) || arg(2)) && (found->second.mode&0x30U))status=0xc000000d;
        else if((control || directory) && arg(1) && (runtime_.wait_objects.find(arg(1))==runtime_.wait_objects.end() || runtime_.wait_objects.at(arg(1)).kind!=WaitObject::Kind::Event))status=0xc0000008;
        else if(control)status=0xc0000010; // 現有一般檔案後端沒有裝置驅動程式的 IOCTL。
        else if(symbol=="NtSetInformationFile") {
            const auto position_guard=runtime_.win_file_positions.at(found->second.object).lock();
            auto& file=found->second;const auto kind=u32(4),size=u32(3);const auto input=arg(2);
            if(kind==39){
                if(size<8)status=0xc0000004;
                else {const auto error=set_file_valid_data(arg(0),memory.load(GuestAddress{input},64));status=error==1314?0xc0000061:nt_error(error);}
            }else if(kind==14){
                if(size<8)status=0xc0000004;
                else {const auto offset=memory.load(GuestAddress{input},64);if(offset>INT64_MAX || !(file.mode&0x30U))status=0xc000000d;else runtime_.win_file_positions.at(file.object)=offset;}
            } else {
                const auto mapped=kind==4?0U:kind==10?3U:kind==13?4U:kind==19?5U:kind==20?6U:UINT32_MAX;
                if(mapped==UINT32_MAX)status=0xc0000003;
                else {const auto error=set_file_information(memory,arg(0),mapped,input,size);status=error==24?0xc0000004:nt_error(error);}
            }
            if(!status && file.native)file.metadata=file.native->information().metadata;
        } else if(symbol=="NtQueryVolumeInformationFile") {
            const auto kind=u32(4),capacity=u32(3);const auto output=arg(2);const auto& file=found->second;
            const unsigned size=kind==1?24U:kind==3?24U:kind==7?32U:kind==4?8U:kind==5?16U:0U;
            if(!size)status=0xc0000003;else if(capacity<size)status=0xc0000004;
            else if(file.pipe && kind!=4)status=0xc0000010;
            else {
                FileVolumeSpace space;if(kind==3 || kind==7){auto path=file.path;if(!(file.metadata.attributes&0x10)){const auto slash=path.find_last_of("\\/");path=slash==path.npos?".":path.substr(0,slash+1);}space=file.native?file.native->volume_space():filesystem_.volume_space(path);status=nt_error(space.error);if(!status && (!space.allocation_unit || !space.sector_size))status=0xc00000bb;}
                FileInformation volume;
                if(kind==1){
                    const auto location=filesystem_.volume_location(file.path.empty()?file.key:file.path);
                    status=nt_error(location.error);
                    if(!status){volume=filesystem_.information(windows_volume_path(location.volume));status=nt_error(volume.error);}
                }
                if(!status){
                    std::vector<std::uint8_t> bytes(kind==1?18U:kind==5?12U:size);
                    const auto put=[&](unsigned offset,unsigned length,std::uint64_t value){for(unsigned n=0;n<length;++n)bytes.at(offset+n)=static_cast<std::uint8_t>(value>>(n*8));};
                    if(kind==3 || kind==7){put(0,8,space.total/space.allocation_unit);put(8,8,space.available/space.allocation_unit);if(kind==7)put(16,8,space.free/space.allocation_unit);put(kind==7?24:16,4,space.allocation_unit/space.sector_size);put(kind==7?28:20,4,space.sector_size);}
                    else if(kind==4){put(0,4,file.pipe?0x11U:7U);put(4,4,file.pipe?0U:0x20U);}
                    else {
                        // 與 Win32 volume 查詢共用虛擬卷名、裝置序號及檔案系統能力。
                        auto name=utf16_bytes(u"X64ARM");name.resize(name.size()-2);
                        if(kind==1){put(0,8,volume.metadata.created);put(8,4,volume.metadata.device);put(12,4,name.size());}
                        else {put(0,4,2|4|(filesystem_.writable()?0U:0x80000U));put(4,4,255);put(8,4,name.size());}
                        bytes.insert(bytes.end(),name.begin(),name.end());
                        if(bytes.size()>capacity){bytes.resize(capacity&~1U);status=0x80000005U;}
                    }
                    memory.write(GuestAddress{output},bytes);transferred=bytes.size();
                }
            }
        } else {
            auto& file=found->second;const auto capacity=u32(6),kind=u32(7);const auto output=arg(5);
            const unsigned fixed=kind==1?64U:kind==2?68U:kind==3?94U:kind==12?12U:kind==37?104U:kind==38?80U:kind==60?88U:kind==63?114U:0U;
            const auto flags=extended?u32(8):0U;
            const bool stateless=(flags&0x10U)!=0,first=stateless || !file.directory_pattern.has_value();
            const bool restart=extended?(flags&0x11U)!=0:arg(10)!=0;
            const bool single=extended?(flags&2U)!=0:arg(8)!=0;
            auto pattern=file.directory_pattern.value_or(u"*");
            if(!(file.metadata.attributes&0x10))status=0xc0000103;
            else if(!(file.access&(0x80000000U|1U)))status=0xc0000022;
            // INDEX_SPECIFIED 只能隨自建 IRP 傳入；此使用者模式入口沒有索引參數。
            else if(flags&~0x1bU)status=0xc000000d;
            else if(!fixed)status=0xc0000003;
            else if(capacity<fixed)status=0xc0000023;
            else {
                if(first){
                    pattern=u"*";
                    if(arg(9)){
                        const auto count=memory.load(GuestAddress{arg(9)},16),maximum=memory.load(GuestAddress{arg(9)+2},16);
                        const auto pointer=memory.load(GuestAddress{arg(9)+width},bits);
                        if((count&1) || count>maximum)status=0xc000000d;
                        else {pattern.clear();for(std::uint64_t i=0;i<count;i+=2)pattern+=static_cast<char16_t>(memory.load(GuestAddress{pointer+i},16));}
                    }
                    if(!status && !stateless)file.directory_pattern=pattern;
                }
                if(!status && !file.directory_loaded){auto entries=filesystem_.list(file.path);status=nt_error(entries.error);if(!status){file.entries=std::move(entries.entries);file.directory_loaded=true;}}
                if(!status){
                    auto cursor=(first || restart)?0:file.directory_cursor;std::vector<std::uint8_t> bytes;std::size_t previous=0;
                    const auto put=[&](std::size_t at,std::uint64_t value,unsigned count){for(unsigned i=0;i<count;++i)bytes.at(at+i)=static_cast<std::uint8_t>(value>>(i*8));};
                    while(cursor<file.entries.size()){
                        const auto& entry=file.entries[cursor];if(!find_matches(pattern,utf8_to_utf16(entry.name),false)){++cursor;continue;}
                        auto name=utf16_bytes(utf8_to_utf16(entry.name));name.resize(name.size()-2);const auto length=fixed+name.size(),offset=(bytes.size()+7)&~std::size_t{7};
                        const bool overflow=offset>capacity || length>capacity-offset;
                        if(overflow && (!bytes.empty() || !first))break;
                        bytes.resize(offset+(overflow?capacity-offset:length));if(offset)put(previous,offset-previous,4);previous=offset;
                        const auto& info=entry.metadata;
                        put(offset+4,cursor,4);
                        if(kind==12)put(offset+8,name.size(),4);
                        else {put(offset+8,info.created,8);put(offset+16,info.accessed,8);put(offset+24,info.modified,8);put(offset+32,info.changed,8);put(offset+40,info.size,8);put(offset+48,info.allocation_size,8);put(offset+56,info.attributes,4);put(offset+60,name.size(),4);if(kind==37)put(offset+96,info.id,8);if(kind==38 || kind==60 || kind==63)put(offset+72,info.id,8);if(kind==60 || kind==63)put(offset+68,info.reparse_tag,4);}
                        std::copy_n(name.begin(),std::min(name.size(),bytes.size()-offset-fixed),bytes.begin()+static_cast<std::ptrdiff_t>(offset+fixed));
                        if(overflow){status=0x80000005;break;}++cursor;if(single)break;
                    }
                    if(bytes.empty() && cursor==file.entries.size())status=first?0xc000000f:0x80000006;
                    if(!bytes.empty()){memory.write(GuestAddress{output},bytes);transferred=bytes.size();}
                    if(!stateless)file.directory_cursor=cursor;
                }
            }
        }
        if(directory && (status==0 || status==0x80000005U || status==0x80000006U || status==0xc000000fU)){
            PendingFileIo completion;completion.handle=arg(0);completion.thread=threads_.current;
            completion.event=arg(1);completion.routine=arg(2);completion.native_context=arg(3);completion.native_iosb=iosb;
            completion.result.bytes=transferred;completion.result.error=status==0?0U:status==0x80000005U?234U:status==0x80000006U?18U:2U;
            complete_file_io(memory,completion,status);
        }else {memory.store(GuestAddress{iosb},bits,status);memory.store(GuestAddress{iosb+width},bits,transferred);}
        threads_.items.at(threads_.current).last_native_status=status;
        arg.return_to(state,status);return std::nullopt;
    }
    if(symbol=="DeviceIoControl"){
        const auto found=files_.find(arg(0));
        if(found==files_.end())fail(6);
        else if(!arg(6) && !arg(7))fail(87);
        else {if(arg(6))memory.store(GuestAddress{arg(6)},32,0);fail(1);} // 一般檔案沒有任意裝置 IOCTL；未建立裝置時不回傳虛構硬體資料。
        arg.return_to(state,result);return std::nullopt;
    }
    if(symbol=="CreateSymbolicLinkA" || symbol=="CreateSymbolicLinkW"){
        return invoke_reparse(state,memory,service);
    }
    if(symbol=="CreateHardLinkA" || symbol=="CreateHardLinkW" || symbol=="PathFileExistsA" || symbol=="PathFileExistsW"){
        const bool link=symbol.starts_with("CreateHardLink"),wide=symbol.ends_with('W');
        if(link && !runtime_.network.external_io)throw HostCallFault("比較模式不允許 CreateHardLink 宿主寫入");
        try{
            if(!arg(0) || (link && (!arg(1) || arg(2))))fail(87);
            else {const auto text=[&](unsigned i){return wide?read_utf16(memory,GuestAddress{arg(i)}):utf8_to_utf16(read_ansi(memory,arg(i),32767));};
                const auto path=text(0);const auto existing=link?text(1):std::u16string{};
                const auto long_path=[](const std::u16string& value){return value.size()>=260 && !value.starts_with(u"\\\\?\\");};
                if(path.empty() || (link && existing.empty()))fail(2);
                else if(link && (long_path(path) || long_path(existing)))fail(3);
                else if(!link && path.size()>=260)fail(206);
                else if(link){const auto error=filesystem_.hard_link(utf16_to_utf8(existing),utf16_to_utf8(path));if(error)fail(error);else result=1;}
                else {const auto info=filesystem_.information(utf16_to_utf8(path));if(info.error)fail(info.error);else result=1;}
            }
        }catch(const MemoryFault&){fail(998);}catch(const std::invalid_argument&){fail(87);}
        arg.return_to(state,result);return std::nullopt;
    }
    if(symbol=="CopyFile2"){
        const auto error=[&](std::uint32_t value){arg.return_to(state,0x80070000U|(value&0xffffU));return std::optional<std::uint32_t>{};};
        if(!arg(0) || !arg(1))return error(87);
        try{
            std::uint32_t flags=0;std::uint64_t cancel=0,callback=0,context=0;
            if(arg(2)){
                const auto p=arg(2);const unsigned unit=x86_?4U:8U;
                if(memory.load(GuestAddress{p},32)!=(x86_?20U:32U))return error(87);
                flags=static_cast<std::uint32_t>(memory.load(GuestAddress{p+4},32));
                cancel=memory.load(GuestAddress{p+8},static_cast<std::uint8_t>(unit*8));
                callback=memory.load(GuestAddress{p+8+unit},static_cast<std::uint8_t>(unit*8));
                context=memory.load(GuestAddress{p+8+2*unit},static_cast<std::uint8_t>(unit*8));
            }
            return start_file_transfer(state,memory,utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)})),utf16_to_utf8(read_utf16(memory,GuestAddress{arg(1)})),flags,callback,context,cancel,false,false,true);
        }catch(const MemoryFault&){return error(998);}catch(const std::bad_alloc&){return error(8);}
    }
    if(symbol=="CopyFileExA" || symbol=="CopyFileExW"){
        if(!arg(0) || !arg(1)){fail(87);arg.return_to(state,0);return std::nullopt;}
        const auto path=[&](unsigned i){return symbol.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(i)})):read_ansi(memory,arg(i));};
        return start_file_transfer(state,memory,path(0),path(1),u32(5),arg(2),arg(3),arg(4));
    }
    if(symbol=="CopyFileA" || symbol=="CopyFileW") {
        if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許 CopyFile 宿主寫入");
        if(!arg(0) || !arg(1))fail(87);
        else {
            const bool wide=symbol.back()=='W';
            const auto path=[&](unsigned index){return wide?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(index)})):read_ansi(memory,arg(index));};
            const auto from=path(0),to=path(1);const auto source=filesystem_.information(from),target=filesystem_.information(to);
            const auto sharing_conflict=[&](const FileInformation& info,std::uint32_t needed,std::uint32_t offered) {
                if(info.error)return false;
                return std::any_of(files_.begin(),files_.end(),[&](const auto& item) {
                    const auto& file=item.second;const auto data=file.native?file.native->information().metadata:file.metadata;
                    if(file.native)return false; // inode／FILE_OBJECT 分享由宿主檔案層統一處理。
                    if(file.key!=info.key && (!data.id || data.id!=info.metadata.id || data.device!=info.metadata.device))return false;
                    const auto used=((file.access&(0x80000000U|1U|0x20U))?1U:0U)|((file.access&(0x40000000U|2U|4U))?2U:0U)|((file.access&0x10000U)?4U:0U);
                    return (needed&~file.sharing) || (used&~offered);
                });
            };
            if(source.error)fail(source.error);
            else if(sharing_conflict(source,1,7) || sharing_conflict(target,2,3))fail(32);
            else if(const auto error=filesystem_.copy(from,to,u32(2)!=0))fail(error);
            else result=1;
        }
    } else if(symbol.starts_with("SetFileAttributes")) {
        const auto path=symbol.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)})):read_ansi(memory,arg(0));
        const auto error=filesystem_.set_attributes(path,u32(1));if(error)fail(error);else result=1;
    } else if(symbol=="FindFirstStreamW") {
        result=UINT64_MAX;
        if(!arg(0) || !arg(2) || u32(1) || u32(3))last_error_=87;
        else if(runtime_.directory_searches.size()>=1024)last_error_=8;
        else {
            const auto path=utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)}));
            const auto file=filesystem_.information(path);
            if(file.error)last_error_=file.error;
            // 目前的映射檔案系統只提供一般檔案的預設資料流；目錄沒有未命名 $DATA。
            else if(file.metadata.attributes&0x10)last_error_=38;
            else {
                DirectorySearch search;search.streams=true;search.entries.push_back({"::$DATA",file.metadata});
                stream_data(memory,arg(2),search.entries.front());search.cursor=1;
                result=runtime_.allocate_wait_handle();runtime_.directory_searches.emplace(result,std::move(search));
            }
        }
    } else if(symbol=="FindNextStreamW") {
        const auto found=runtime_.directory_searches.find(arg(0));
        if(found==runtime_.directory_searches.end() || !found->second.streams)fail(6);
        else if(!arg(1))fail(87);
        else if(found->second.cursor==found->second.entries.size())fail(38);
        else {auto& search=found->second;stream_data(memory,arg(1),search.entries[search.cursor]);++search.cursor;result=1;}
    } else if(symbol=="FindClose") {if(runtime_.directory_searches.erase(arg(0)))result=1;else fail(6);}
    else if(symbol.starts_with("FindNextFile")) {
        const auto found=runtime_.directory_searches.find(arg(0));
        if(found==runtime_.directory_searches.end() || found->second.streams)fail(6);
        else if(found->second.cursor==found->second.entries.size())fail(18);
        else {auto& search=found->second;find_data(memory,arg(1),search.entries[search.cursor],symbol.back()=='W');++search.cursor;result=1;}
    } else if(symbol.starts_with("FindFirstFile")) {
        const bool wide=symbol.back()=='W',extended=symbol.starts_with("FindFirstFileEx");
        const auto path=wide?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)})):read_ansi(memory,arg(0));
        result=UINT64_MAX;
        if(extended && (u32(1)>1 || u32(3)>1 || arg(4) || (u32(5)&~7U)))last_error_=87;
        else if(runtime_.directory_searches.size()>=1024)last_error_=8;
        else {
            auto [error,search]=find_directory(filesystem_,path,extended && (u32(5)&1),extended && u32(3)==1);
            if(error)last_error_=error;
            else {
                find_data(memory,arg(extended?2:1),search.entries[0],wide);search.cursor=1;result=runtime_.allocate_wait_handle();runtime_.directory_searches.emplace(result,std::move(search));
            }
        }
    } else if (symbol=="GetFileTime" || symbol=="GetFileSize") {
        const auto file=file_handle_information(arg(0));
        if(file.error){fail(file.error);if(symbol=="GetFileSize")result=UINT32_MAX;}
        else if(symbol=="GetFileSize"){
            if(arg(1))memory.store(GuestAddress{arg(1)},32,file.metadata.size>>32);
            result=static_cast<std::uint32_t>(file.metadata.size);last_error_=0;
        }else {
            const std::array times{file.metadata.created,file.metadata.accessed,file.metadata.modified};
            for(unsigned i=1;i<4;++i)if(arg(i))memory.check(GuestAddress{arg(i)},8,Access::Write);
            for(unsigned i=1;i<4;++i)if(arg(i))memory.store(GuestAddress{arg(i)},64,times[i-1]);result=1;
        }
    } else if (symbol=="SetFileTime") {
        const auto found=files_.find(arg(0));
        if(found==files_.end())fail(6);
        else {
            auto& file=found->second;
            if(!file.native || !(file.access&(0x40000000U|0x100U)))fail(5);
            else {
                const auto value=[&](unsigned i){return arg(i)?memory.load(GuestAddress{arg(i)},64):0;};
                const auto error=file.native->set_times(value(1),value(2),value(3));if(error)fail(error);else result=1;
            }
        }
    } else if (symbol=="SetEndOfFile" || symbol=="FlushFileBuffers" || symbol=="SetFilePointer") {
        const auto found=files_.find(arg(0));
        if(found!=files_.end() && (found->second.pipe || ((found->second.serial || found->second.hid) && symbol=="FlushFileBuffers")))return invoke_file_io(state,memory,service);
        if(found==files_.end()) {fail(6);if(symbol=="SetFilePointer")result=UINT32_MAX;}
        else if(found->second.serial || found->second.hid || found->second.usb){fail(1);if(symbol=="SetFilePointer")result=UINT32_MAX;}
        else {
            auto& file=found->second;if(file.native)file.metadata=file.native->information().metadata;
#if !defined(_WIN32)
            if(symbol=="FlushFileBuffers" && file.native && (file.access&(0x40000000U|2U|4U))){
                // fsync 交給宿主工作執行緒；呼叫端以 FileIo 等待，單核客體期間其他客體執行緒仍可執行，完成時通知排程器。
                const auto previous=std::find_if(pending_file_io_.begin(),pending_file_io_.end(),[&](const auto& io){return io.flush && io.thread==threads_.current && io.handle==arg(0);});
                if(previous!=pending_file_io_.end()){
                    if(!previous->done)throw HostCallFault("檔案 flush 尚未完成卻恢復執行緒");
                    const auto completed=previous->result;pending_file_io_.erase(previous);threads_.items.at(threads_.current).wait={};
                    if(completed.error)fail(completed.error);else{result=1;file.metadata=file.native->information().metadata;}
                    arg.return_to(state,result);return std::nullopt;
                }
                if(const int descriptor=file.native->duplicate_descriptor();descriptor>=0){
                    auto task=std::make_shared<AsyncFileFlush>();auto wake=mapping_wake();
                    try{
                        std::thread([task,descriptor,wake]{
                            const bool failed=::fsync(descriptor)<0;const auto code=errno;::close(descriptor);
                            task->error=!failed?0U:(code==ENOSPC || code==EDQUOT)?112U:code==EROFS?19U:code==EBADF?6U:1117U;
                            task->done.store(true,std::memory_order_release);if(wake)wake->notify();
                        }).detach();
                    }catch(const std::system_error&){::close(descriptor);task.reset();}
                    if(task){
                        PendingFileIo io;io.kind=PendingFileIo::Kind::Flush;io.handle=arg(0);io.thread=threads_.current;io.flush=std::move(task);io.blocking=true;
                        set_file_io_signaled(io.handle,false);pending_file_io_.push_back(std::move(io));
                        auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};
                        thread.wait.kind=ThreadWaitKind::FileIo;thread.wait.handles={arg(0)};
                        schedule(state,memory);return std::nullopt;
                    }
                }
                // 描述元無法複製或工作執行緒無法建立時，沿用同步 fsync。
            }
#endif
            if(symbol=="SetFilePointer") {
                const auto raw=arg(2)?(memory.load(GuestAddress{arg(2)},32)<<32)|u32(1):static_cast<std::uint64_t>(static_cast<std::int64_t>(std::bit_cast<std::int32_t>(u32(1))));
                const auto position_guard=runtime_.win_file_positions.at(file.object).lock();const auto offset=std::bit_cast<std::int64_t>(raw);const __int128 position=static_cast<__int128>(offset)+(u32(3)==1?runtime_.win_file_positions.at(file.object):u32(3)==2?file.metadata.size:0);
                if(u32(3)>2 || position>INT64_MAX || position<0) {fail(position<0?131:87);result=UINT32_MAX;}
                else {if(arg(2))memory.store(GuestAddress{arg(2)},32,static_cast<std::uint64_t>(position)>>32);runtime_.win_file_positions.at(file.object)=static_cast<std::uint64_t>(position);result=static_cast<std::uint32_t>(position);last_error_=0;}
            } else if(!file.native || !(file.access&(0x40000000U|2U|4U)))fail(5);
            else {const auto position_guard=runtime_.win_file_positions.at(file.object).lock();const auto error=symbol=="SetEndOfFile"?file.native->truncate(runtime_.win_file_positions.at(file.object)):file.native->flush();if(error)fail(error);else {result=1;file.metadata=file.native->information().metadata;}}
        }
    } else if(symbol=="ReplaceFileA" || symbol=="ReplaceFileW"){
        if(!runtime_.network.external_io && filesystem_.writable())throw HostCallFault("比較模式不允許宿主檔案替換");
        if(!arg(0)||!arg(1)||arg(4)||arg(5)||(u32(3)&~6U))fail(87);
        else {
            const auto path=[&](unsigned i){return symbol.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(i)})):read_ansi(memory,arg(i));};
            const auto error=filesystem_.replace(path(0),path(1),arg(2)?path(2):std::string{},u32(3));if(error)fail(error);else result=1;
        }
    } else if (symbol.starts_with("CreateDirectory") || symbol.starts_with("RemoveDirectory") || symbol.starts_with("DeleteFile") || symbol.starts_with("MoveFile") ||
        symbol.ends_with("mkdir") || symbol.ends_with("rmdir") || symbol=="remove" || symbol=="_wremove" || symbol.ends_with("unlink") || symbol.ends_with("rename")) {
        if(filesystem_.writable() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許宿主檔案建立、移動或刪除");
        const bool crt=service.family=="crt",wide=crt?symbol.starts_with("_w"):symbol.back()=='W';
        const auto path=[&](unsigned i){return wide?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(i)})):read_ansi(memory,arg(i));};
        const bool template_directory=symbol=="CreateDirectoryExA" || symbol=="CreateDirectoryExW";
        const auto from=template_directory && (!arg(0) || !arg(1))?std::string{}:path(template_directory?1:0);std::uint32_t error=0;
        const bool directory=symbol.starts_with("RemoveDirectory") || symbol.ends_with("rmdir"),moving=symbol.starts_with("MoveFile") || symbol.ends_with("rename");
        if(symbol.starts_with("CreateDirectory") || symbol.ends_with("mkdir")){
            try{
                const auto security_argument=template_directory?2U:1U;
                if(template_directory && (!arg(0) || !arg(1)))error=87;
                else {
                    // kernel32 CreateDirectory 只把 lpSecurityDescriptor 交給 NtCreateFile，不檢查 nLength（.NET 傳 0）。
                    if(!crt)(void)read_security_attributes(memory,arg(security_argument),x86_,false,false);
                    const auto source=template_directory?path(0):std::string{};
                    error=template_directory && source.empty()?3U:filesystem_.create_directory(from,crt?std::function<std::vector<std::uint8_t>()>{}:std::function<std::vector<std::uint8_t>()>{[&]{return file_creation_descriptor(memory,arg(security_argument),false);}},source);
                }
            }catch(const SecurityDescriptorError& failure){error=failure.code;}catch(const MemoryFault&){error=87;}
        }
        else {
            const auto info=filesystem_.information(from);
            if(info.error)error=info.error;
            else if(std::any_of(files_.begin(),files_.end(),[&](const auto& entry){return !entry.second.native && entry.second.key==info.key && !(entry.second.sharing&4U);}))error=32;
            else if(moving) {
                const bool progress=symbol.starts_with("MoveFileWithProgress");
                const auto flags_value=progress?u32(4):symbol.starts_with("MoveFileEx")?u32(2):0;
                if(flags_value&~11U)error=50;
                else {
                    const auto destination=path(1);const auto target=filesystem_.information(destination);
                    if(std::any_of(files_.begin(),files_.end(),[&](const auto& entry){return !entry.second.native && !target.error && entry.second.key==target.key && !(entry.second.sharing&4U);}))error=32;
                    else {
                        // 同一檔案系統的 rename 沒有複製資料，不呼叫 CopyProgressRoutine。
                        error=filesystem_.rename(from,destination,(flags_value&1)!=0,(flags_value&2)!=0 && (!progress || !arg(2)),(flags_value&8)!=0);
                        if(error==17 && progress && arg(2) && (flags_value&2))return start_file_transfer(state,memory,from,destination,(flags_value&1)?0U:1U,arg(2),arg(3),0,true,(flags_value&8)!=0);
                    }
                }
            } else if(!directory && (info.metadata.attributes&0x10))error=5;
            else error=filesystem_.remove(from,directory);
        }
        if(error) {if(crt){crt_os_error::store(memory,runtime_.errno_cell,error);result=UINT32_MAX;}else fail(error);}
        else result=crt?0:1;
    } else if (symbol=="NtQueryInformationFile") {
        const auto found=files_.find(arg(0));
        const bool standard=arg(0)>=0x100 && arg(0)<=0x102 && standard_open_[arg(0)-0x100];
        const auto kind=u32(4),capacity=u32(3);
        std::size_t size=0;std::uint32_t status=0;std::vector<std::uint8_t> bytes;
        const auto access=security::mapped_access(standard?(arg(0)==0x100?0x80000000U:0x40000000U):found==files_.end()?0U:found->second.access,{0x120089U,0x120116U,0x1200a0U,0x1f01ffU});
        if (!standard && found==files_.end()) status=0xc0000008;
        else {
            switch (kind) {
            case 4:size=40;break;case 5:size=24;break;case 6:size=8;break;case 7:case 8:case 16:case 17:size=4;break;case 14:size=8;break;
            case 9:size=8;break;case 18:size=104;break;case 34:size=56;break;case 35:size=8;break;
            default:status=0xc0000003;break;
            }
            if (!status && capacity<size) status=0xc0000004;
            if (!status && (kind==4 || kind==18 || kind==34 || kind==35) && !(access&0x80U))status=0xc0000022;
            if (!status && standard && (kind==9 || kind==18))status=0xc0000010;
            if (!status && found!=files_.end() && found->second.native) {
                const auto current=found->second.native->information();
                if(current.error)status=current.error==5?0xc0000022U:current.error==6?0xc0000008U:0xc0000001U;
                else found->second.metadata=current.metadata;
            }
        }
        memory.check(GuestAddress{arg(1)},x86_?8:16,Access::Write);
        if (!status) {
            bytes.resize(size);
            const auto& info=standard?FileMetadata{}:found->second.metadata;
            const auto store=[&](unsigned offset,unsigned width,std::uint64_t value){for(unsigned n=0;n<width/8;++n)bytes.at(offset+n)=static_cast<std::uint8_t>(value>>(n*8));};
            // 單項與 All 使用相同的欄位契約；x86／x64 的 FILE_ALL_INFORMATION 固定區皆為 100 bytes。
            const auto fill=[&](std::uint32_t part,unsigned offset) {
                switch(part) {
                case 4:store(offset,64,info.created);store(offset+8,64,info.accessed);store(offset+16,64,info.modified);store(offset+24,64,info.changed);store(offset+32,32,info.attributes);break;
                case 5:store(offset,64,info.allocation_size);store(offset+8,64,info.size);store(offset+16,32,info.links);store(offset+20,8,!standard && found->second.native && found->second.native->delete_pending());store(offset+21,8,(info.attributes&0x10)!=0);break;
                case 6:store(offset,64,info.id);break;
                case 8:store(offset,32,access);break;
                case 14:store(offset,64,standard?0:static_cast<std::uint64_t>(runtime_.win_file_positions.at(found->second.object)));break;
                case 16:store(offset,32,standard?0x20:found->second.mode);break;
                default:break; // 未提供 NT EA；一般映射檔案支援 byte alignment。
                }
            };
            if(kind==18) {
                fill(4,0);fill(5,40);fill(6,64);fill(7,72);fill(8,76);fill(14,80);fill(16,88);fill(17,92);
            } else if(kind==34) {
                fill(4,0);store(32,64,info.allocation_size);store(40,64,info.size);store(48,32,info.attributes);
            } else if(kind==35) {
                store(0,32,info.attributes);store(4,32,(info.attributes&0x400U)?info.reparse_tag:0U);
            } else fill(kind,0);
            if(kind==9 || kind==18) {
                auto [name,error]=name_information(found->second);
                if(error){status=error==3?0xc000003aU:0xc0000001U;bytes.clear();}
                else {
                    const unsigned offset=kind==18?96U:0U;
                    bytes.resize(offset);bytes.insert(bytes.end(),name.begin(),name.end());
                    // 保留完整名稱長度，短緩衝只複製完整 WCHAR；警告仍提供所有已填入的 metadata。
                    if(bytes.size()>capacity){bytes.resize(capacity&~1U);status=0x80000005U;}
                }
            }
            if(!bytes.empty())memory.write(GuestAddress{arg(2)},bytes);
        }
        if(!bytes.empty())process_io::record(process_io::Kind::Other,bytes.size());
        memory.store(GuestAddress{arg(1)},x86_?32:64,status);memory.store(GuestAddress{arg(1)+(x86_?4U:8U)},x86_?32:64,bytes.size());result=status;
    } else if (symbol=="GetVolumeInformationByHandleW") {
        const auto found=files_.find(arg(0));
        if (found==files_.end()) fail(6);
        else if ((arg(1) && u32(2)<7) || (arg(6) && u32(7)<7)) fail(122);
        else {
            if (arg(1)) memory.check(GuestAddress{arg(1)},14,Access::Write);if (arg(6)) memory.check(GuestAddress{arg(6)},14,Access::Write);
            for (unsigned i=3;i<=5;++i) if (arg(i)) memory.check(GuestAddress{arg(i)},4,Access::Write);
            if (arg(1)) memory.write(GuestAddress{arg(1)},utf16_bytes(u"X64ARM"));if (arg(6)) memory.write(GuestAddress{arg(6)},utf16_bytes(u"X64ARM"));
            if (arg(3)) memory.store(GuestAddress{arg(3)},32,found->second.metadata.device);if (arg(4)) memory.store(GuestAddress{arg(4)},32,255);
            if (arg(5)) memory.store(GuestAddress{arg(5)},32,2|4|(filesystem_.writable()?0U:0x80000U));result=1;
        }
    } else if (symbol=="GetFileInformationByHandleEx") {
        const auto found=files_.find(arg(0));
        const auto kind=u32(1),capacity=u32(3);const auto output=arg(2);
        // 名稱由檔案物件提供；命名管線也有名稱，不要求磁碟 metadata。
        const auto queried=kind==2 && found!=files_.end()?FileInformation{}:file_handle_information(arg(0));
        if (queried.error) fail(queried.error);
        else if (found==files_.end() && (kind==2 || kind==10 || kind==11 || kind==14 || kind==15))fail(50);
        else if(kind==2) {
            if(capacity<4)fail(24);else if(!output)fail(87);
            else {auto [bytes,error]=name_information(found->second);
                if(error)fail(error);
                else {const auto count=std::min<std::size_t>(capacity,bytes.size());memory.write(GuestAddress{output},std::span{bytes}.first(count));if(count<bytes.size())fail(234);else result=1;}}
        }
        else if (kind==10 || kind==11 || kind==14 || kind==15) {
            auto& file=found->second;
            if (!(file.metadata.attributes&0x10)) fail(87);
            else {
                if (!file.directory_loaded) {
                    const auto entries=filesystem_.list(file.path);
                    if (entries.error) {fail(entries.error);arg.return_to(state,result);return std::nullopt;}
                    file.entries=entries.entries;file.directory_loaded=true;
                }
                const bool restart=kind==11 || kind==15,both=kind==10 || kind==11;
                auto cursor=restart?0:file.directory_cursor;std::vector<std::uint8_t> bytes;
                const unsigned fixed=both?104U:68U;
                std::size_t previous=0;
                const auto put=[&](std::size_t at,std::uint64_t value,unsigned width) {for (unsigned i=0;i<width;++i) bytes.at(at+i)=static_cast<std::uint8_t>(value>>(i*8));};
                while (cursor<file.entries.size()) {
                    const auto& entry=file.entries[cursor];auto name=utf16_bytes(utf8_to_utf16(entry.name));name.resize(name.size()-2);const auto extent=(fixed+name.size()+7)&~std::size_t{7};
                    if (extent>capacity-bytes.size()) break;
                    const auto offset=bytes.size();bytes.resize(offset+extent);
                    if (offset) put(previous,offset-previous,4);previous=offset;const auto& info=entry.metadata;
                    put(offset+8,info.created,8);put(offset+16,info.accessed,8);put(offset+24,info.modified,8);put(offset+32,info.changed,8);
                    put(offset+40,info.size,8);put(offset+48,info.allocation_size,8);put(offset+56,info.attributes,4);put(offset+60,name.size(),4);
                    if (both) put(offset+96,info.id,8);std::copy(name.begin(),name.end(),bytes.begin()+static_cast<std::ptrdiff_t>(offset+fixed));++cursor;
                }
                if (bytes.empty()) fail(cursor==file.entries.size()?18:234);
                else {memory.write(GuestAddress{output},bytes);file.directory_cursor=cursor;process_io::record(process_io::Kind::Other,bytes.size());result=1;}
            }
        } else {
            const auto& info=queried.metadata;const unsigned extent=kind==0?40U:kind==1 || kind==18?24U:kind==9?8U:0;
            if (!extent) fail(87);else if (capacity<extent) fail(24);else if(!output)fail(87);
            else {
                memory.write(GuestAddress{output},std::vector<std::uint8_t>(extent));
                if (kind==0) {memory.store(GuestAddress{output},64,info.created);memory.store(GuestAddress{output+8},64,info.accessed);memory.store(GuestAddress{output+16},64,info.modified);memory.store(GuestAddress{output+24},64,info.changed);memory.store(GuestAddress{output+32},32,info.attributes);}
                else if (kind==1) {memory.store(GuestAddress{output},64,info.allocation_size);memory.store(GuestAddress{output+8},64,info.size);memory.store(GuestAddress{output+16},32,info.links);memory.store(GuestAddress{output+20},8,found!=files_.end() && found->second.native && found->second.native->delete_pending());memory.store(GuestAddress{output+21},8,(info.attributes&0x10)!=0);}
                else if (kind==9) {memory.store(GuestAddress{output},32,info.attributes);if (info.attributes&0x400) memory.store(GuestAddress{output+4},32,info.reparse_tag?info.reparse_tag:0xa000000cU);}
                else {memory.store(GuestAddress{output},64,info.device);memory.store(GuestAddress{output+8},64,info.id);}
                process_io::record(process_io::Kind::Other,extent);result=1;
            }
        }
    } else if (symbol=="GetFileInformationByHandle") {
        const auto file=file_handle_information(arg(0));
        if (file.error) fail(file.error);
        else if(!arg(1))fail(87);
        else {
            const auto& info=file.metadata;
            memory.check(GuestAddress{arg(1)},52,Access::Write);attributes(arg(1),info);
            memory.store(GuestAddress{arg(1)+28},32,info.device);memory.store(GuestAddress{arg(1)+32},32,info.size>>32);memory.store(GuestAddress{arg(1)+36},32,info.size);
            memory.store(GuestAddress{arg(1)+40},32,info.links);memory.store(GuestAddress{arg(1)+44},32,info.id>>32);memory.store(GuestAddress{arg(1)+48},32,info.id);process_io::record(process_io::Kind::Other,52);result=1;
        }
    } else {
        const bool crt=service.family=="crt",wide=crt?symbol.starts_with("_w"):symbol.back()=='W';
        const bool descriptor=symbol.starts_with("_fstat");bool pipe=false;
        FileInformation file;
        if(descriptor) {
            const auto fd=u32(0);const auto found=runtime_.descriptors.find(fd);
            if(fd<3) {if(!standard_open_[fd])file.error=6;else pipe=true;}
            else if(found==runtime_.descriptors.end())file.error=6;
            else {const auto& stream=runtime_.streams.at(found->second);pipe=stream.standard_handle!=0;
                if(stream.native)file=stream.native->information();else file.metadata=stream.metadata;}
            if(pipe){file.metadata.created=file.metadata.accessed=file.metadata.modified=UINT64_C(116444736000000000);file.metadata.attributes=0x80;file.metadata.links=1;}
        } else {
            const auto path=wide?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)})):read_ansi(memory,arg(0));
            file=filesystem_.information(path);
        }
        if (file.error) {
            if (crt) {crt_os_error::store(memory,runtime_.errno_cell,file.error);result=UINT32_MAX;}
            else {fail(file.error);if (symbol=="GetFileAttributesA" || symbol=="GetFileAttributesW") result=UINT32_MAX;}
        } else if (!crt) {
            if (symbol=="GetFileAttributesA" || symbol=="GetFileAttributesW") result=file.metadata.attributes;
            else if (u32(1)) fail(87);
            else {attributes(arg(2),file.metadata);result=1;}
        } else {
            const auto& info=file.metadata;
            // MSVCRT 的 _stat／_stati64 使用 32 位元 time_t；顯式 64 版本才使用 64 位元時間。
            const bool time64=(symbol.ends_with("64") && !symbol.ends_with("i64")) || symbol.ends_with("64i32");
            const bool size64=symbol.ends_with("i64") || (symbol.ends_with("64") && !symbol.ends_with("64i32"));
            const unsigned time_start=size64?32U:24U,time_width=time64?64U:32U;
            const unsigned extent=time64?(size64?56U:48U):(size64?48U:36U);
            const auto seconds=[](std::uint64_t value){return (static_cast<__int128>(value)-UINT64_C(116444736000000000))/10000000;};
            if ((!size64 && info.size>INT32_MAX) || (!time64 && (seconds(info.modified)>INT32_MAX || seconds(info.accessed)>INT32_MAX || seconds(info.created)>INT32_MAX))) {
                memory.store(GuestAddress{runtime_.errno_cell},32,34);result=UINT32_MAX;
            } else {
                memory.write(GuestAddress{arg(1)},std::vector<std::uint8_t>(extent));
                const auto mode=(pipe?0x1000U:info.attributes&0x10?0x4000U:0x8000U)|0x124U|((info.attributes&1)?0:0x92U)|((info.attributes&0x10)?0x49U:0);
                const auto device=pipe?u32(0):2;
                memory.store(GuestAddress{arg(1)},32,device);memory.store(GuestAddress{arg(1)+6},16,mode);
                memory.store(GuestAddress{arg(1)+8},16,info.links);memory.store(GuestAddress{arg(1)+16},32,device);
                memory.store(GuestAddress{arg(1)+(size64?24U:20U)},size64?64:32,info.size);
                const std::array times{info.accessed,info.modified,info.created};
                for (unsigned i=0;i<3;++i) memory.store(GuestAddress{arg(1)+time_start+i*(time_width/8)},static_cast<std::uint8_t>(time_width),static_cast<std::uint64_t>(seconds(times[i])));
            }
        }
    }
    arg.return_to(state,result);return std::nullopt;
}
}
