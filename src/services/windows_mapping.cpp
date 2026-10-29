#include "x64arm/windows_services.hpp"
#include "x64arm/cross_architecture.hpp"
#include "x64arm/shared_event.hpp"
#include "x64arm/shared_wake.hpp"
#include "x64arm/windows_security_attributes.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_paths.hpp"
#include "x64arm/crypto.hpp"
#include "x64arm/native_font_data.hpp"
#include "x64arm/pe_section_image.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <system_error>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "mapping_file_broker.hpp"
#endif

namespace x64arm {
namespace {
constexpr std::uint64_t granularity = 65536, data_offset = 65536;
constexpr std::uint64_t magic = 0x50414d4d52413458ULL;
std::uint32_t os_error() { return errno == EACCES || errno == EPERM ? 5U : errno == ENOENT ? 2U : errno == ENOMEM || errno == ENOSPC ? 8U : 87U; }
#if !defined(_WIN32)
struct Session {
    std::filesystem::path path;
    std::shared_ptr<SharedWake> wake;
    int directory{-1},lease{-1}; bool owner{};
    ~Session();
};
struct DirectoryLock {
    int descriptor;
    explicit DirectoryLock(int value) : descriptor(value) {
        while (::flock(descriptor, LOCK_EX) < 0) { if (errno != EINTR) throw HostCallFault("無法鎖定共用映射命名空間"); }
    }
    ~DirectoryLock() { (void)::flock(descriptor, LOCK_UN); }
};
Session::~Session() {
    // 關閉自己的 shared lease；不可 LOCK_UN，因為尚未完成啟動的子程序
    // 可能持有相同 open-file description 的繼承副本。
    if(lease>=0)::close(lease);
    if(owner && directory>=0)try {
        DirectoryLock lock(directory);
        const auto probe=::openat(directory,".lifetime",O_RDWR|O_CLOEXEC|O_NOFOLLOW);
        if(probe>=0){
            if(!::flock(probe,LOCK_EX|LOCK_NB)){
                std::error_code ignored;std::filesystem::remove_all(path,ignored);
            }
            ::close(probe);
        }
    }catch(...){ /* 解構不拋例外；異常退出遺留檔案仍在私有命名空間內。 */ }
    if(directory>=0)::close(directory);
}
struct Header {
    std::uint64_t signature{magic}, size{}, references{};
    std::uint32_t protection{}, characters{};
    std::array<char16_t, 260> name{};
};
std::shared_ptr<Session> session(std::string path = {},int inherited_descriptor=-1) {
    auto result = std::make_shared<Session>();
    if (path.empty()) {
        auto pattern = (std::filesystem::temp_directory_path() / "x64arm-mapping-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end()); buffer.push_back(0);
        const auto made = ::mkdtemp(buffer.data());
        if (!made) throw HostCallFault("無法建立共用映射命名空間");
        path = made; result->owner = true;
    }
    result->path = path;
    result->directory = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat info{};
    if (result->directory < 0 || ::fstat(result->directory, &info) || info.st_uid != ::geteuid() || (info.st_mode & 077) != 0)
        throw HostCallFault("共用映射命名空間權限無效");
    DirectoryLock lock(result->directory);
    const std::array<std::uint64_t,2> initial{UINT64_C(0x31534e4d52413458),result->owner?1U:0U};
    result->lease=::openat(result->directory,".lifetime",O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(result->lease>=0){
        if(::write(result->lease,initial.data(),sizeof(initial))!=sizeof(initial))throw HostCallFault("無法建立命名空間生命週期");
    }else if(errno==EEXIST)result->lease=::openat(result->directory,".lifetime",O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    std::array<std::uint64_t,2> header{};struct stat lease_info{};
    if(result->lease<0 || ::fstat(result->lease,&lease_info) || !S_ISREG(lease_info.st_mode) || lease_info.st_uid!=::geteuid() ||
       (lease_info.st_mode&077) || lease_info.st_size!=sizeof(header) || ::pread(result->lease,header.data(),sizeof(header),0)!=sizeof(header) ||
       header[0]!=initial[0] || header[1]>1)throw HostCallFault("命名空間生命週期權限無效");
    if(inherited_descriptor>=0){
        struct stat inherited{};
        if(::fstat(inherited_descriptor,&inherited) || inherited.st_dev!=lease_info.st_dev || inherited.st_ino!=lease_info.st_ino)
            throw HostCallFault("繼承的命名空間生命週期不相符");
    }
    while(::flock(result->lease,LOCK_SH)<0)if(errno!=EINTR)throw HostCallFault("無法保留共用命名空間");
    // 只有引擎 mkdtemp 建立的命名空間可由最後一個參與者回收。
    result->owner=header[1]!=0;result->wake=SharedWake::open(path);
    return result;
}
int font_directory(const Session& ns) {
    if(::mkdirat(ns.directory,"fonts",0700) && errno!=EEXIST)return -1;
    const auto descriptor=::openat(ns.directory,"fonts",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    struct stat details{};
    if(descriptor<0)return -1;
    if(::fstat(descriptor,&details) || details.st_uid!=::geteuid() || (details.st_mode&077)){::close(descriptor);errno=EACCES;return -1;}
    return descriptor;
}
std::string object_name(std::u16string_view name) {
    std::uint64_t a = 14695981039346656037ULL, b = 0x84222325cbf29ce4ULL;
    for (auto c : name) { a = (a ^ c) * 1099511628211ULL; b = (b ^ (c + 1U)) * 0x100000001b3ULL; }
    std::ostringstream out; out << "section-" << std::hex << std::setw(16) << std::setfill('0') << a << std::setw(16) << b; return out.str();
}
struct MappingWindow {
    void* data{MAP_FAILED};std::uint64_t size{};
    MappingWindow(std::uint64_t bytes,int flags,int descriptor,std::uint64_t offset):size(bytes){
        data=::mmap(nullptr,static_cast<std::size_t>(size),PROT_READ|PROT_WRITE,flags,descriptor,static_cast<off_t>(offset));
    }
    ~MappingWindow(){if(data!=MAP_FAILED)::munmap(data,static_cast<std::size_t>(size));}
};
#include "windows_image_section_storage.inc"
struct Mapping {
    std::shared_ptr<Session> name_space;
    std::shared_ptr<void> file_lifetime;
    std::string name;
    std::u16string path;
    int descriptor{-1},file_descriptor{-1};
    std::unique_ptr<mapping_detail::Broker> broker;
    std::shared_ptr<MappingWindow> window;
    std::uint64_t size{};
    std::uint32_t protection{};
    bool named{}, counted{}, persistent{};
    std::weak_ptr<MemoryBacking> backing;
    std::optional<PeSectionImage> image;
    ~Mapping() {
        const auto endpoint=broker?broker->name:std::string{};broker.reset();
        if ((counted || !endpoint.empty()) && descriptor >= 0) {
            try {
                DirectoryLock lock(name_space->directory); Header header{};
                if(!endpoint.empty()){
                    mapping_detail::FileRecord record{};
                    if(::pread(descriptor,&record,sizeof(record),mapping_detail::file_record_offset)==sizeof(record) && record.signature==mapping_detail::file_magic){
                        for(auto& entry:record.endpoints)if(std::string_view(entry.data(),strnlen(entry.data(),entry.size()))==endpoint)entry.fill(0);
                        (void)::pwrite(descriptor,&record,sizeof(record),mapping_detail::file_record_offset);
                    }
                }
                if (counted && ::pread(descriptor, &header, sizeof(header), 0) == sizeof(header) && header.signature == magic && header.references) {
                    --header.references;
                    if (header.references || persistent) (void)::pwrite(descriptor, &header, sizeof(header), 0);
                    else (void)::unlinkat(name_space->directory, name.c_str(), 0);
                }
            } catch (...) { /* 解構不拋例外；程序異常結束的檔案留待 session 清理。 */ }
        }
        if(file_descriptor>=0)::close(file_descriptor);
        if (descriptor >= 0) ::close(descriptor);
    }
};
int mapping_window_flags(const Mapping& object){
    return object.file_descriptor>=0 && (object.protection&0xffU)!=4 && (object.protection&0xffU)!=0x40U?MAP_PRIVATE:MAP_SHARED;
}
// 呼叫者持有 namespace 鎖。舊 MemoryBacking 自行保留原映射，不搬移已發布的位址。
std::uint32_t refresh_mapping_window(Mapping& object,const Header& header){
    if(header.signature!=magic || !header.size || header.size>GuestMemory::maximum_capacity || header.size<object.size)return 6;
    if(header.size==object.size)return 0;
    if(object.image)return 6;
    auto window=std::make_shared<MappingWindow>(header.size,mapping_window_flags(object),
        object.file_descriptor>=0?object.file_descriptor:object.descriptor,object.file_descriptor>=0?0:data_offset);
    if(window->data==MAP_FAILED)return os_error();
    object.window=std::move(window);object.size=header.size;object.backing.reset();return 0;
}
std::uint32_t refresh_mapping_size(Mapping& object){
    DirectoryLock lock(object.name_space->directory);Header header{};
    if(::pread(object.descriptor,&header,sizeof(header),0)!=sizeof(header))return 6;
    return refresh_mapping_window(object,header);
}
// 呼叫者持有 namespace 鎖；檔案資料與 section 中繼資料分開管理。
std::uint32_t attach_mapping_file(Mapping& object,mapping_detail::FileRecord& record,int source=-1){
    if(record.signature!=mapping_detail::file_magic)return 0;
    object.file_descriptor=source>=0?::fcntl(source,F_DUPFD_CLOEXEC,0):mapping_detail::receive(object.name_space->path,record);
    if(object.file_descriptor<0)return 6;
    const auto end=std::find(record.path.begin(),record.path.end(),char16_t{});
    if(end==record.path.end())return 6;object.path.assign(record.path.begin(),end);
    auto slot=std::find_if(record.endpoints.begin(),record.endpoints.end(),[](const auto& entry){return entry[0]==0;});
    if(slot==record.endpoints.end())return 8;
    object.broker=std::make_unique<mapping_detail::Broker>(object.name_space->path,object.file_descriptor);
    std::copy(object.broker->name.begin(),object.broker->name.end(),slot->begin());
    if(::pwrite(object.descriptor,&record,sizeof(record),mapping_detail::file_record_offset)!=sizeof(record))return os_error();
    return 0;
}
struct MappingFileInput {int descriptor;std::u16string path;std::shared_ptr<void> lifetime;};
struct Opened { std::shared_ptr<Mapping> mapping; std::uint32_t error{}; bool existed{}; };
Opened open_mapping(const std::shared_ptr<Session>& ns, std::u16string_view name, std::uint64_t size, std::uint32_t protection, bool create,const std::vector<std::uint8_t>& security,const MappingFileInput* file=nullptr,std::size_t image_limit=GuestMemory::capacity_limit,std::span<const std::uint8_t> initial={},bool internal=false) {
    if (name.size() >= 260 || name.find(u'\\', name.starts_with(u"Local\\") ? 6 : name.starts_with(u"Global\\") ? 7 : 0) != std::u16string_view::npos)
        return {{}, 123};
    auto result = std::make_shared<Mapping>(); result->name_space = ns; result->named = !name.empty();
    // 全命名空間鎖同時涵蓋 open / reference / unlink，避免 last-close 與再 open 競態。
    DirectoryLock lock(ns->directory);
    result->name = result->named ? object_name(name) : "anonymous-" + std::to_string(::getpid()) + "-" + std::to_string(reinterpret_cast<std::uintptr_t>(result.get()));
    if(internal){result->name="table-"+result->name;result->persistent=true;} // 與客體可開啟的命名 section 分離。
    bool created = false;
    if (create) {
        result->descriptor = ::openat(ns->directory, result->name.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        created = result->descriptor >= 0;
        if (!created && errno != EEXIST) return {{}, os_error()};
    }
    if (result->descriptor < 0) result->descriptor = ::openat(ns->directory, result->name.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (result->descriptor < 0) return {{}, os_error()};
    struct PendingName {
        int directory;const std::string& name;bool enabled;
        ~PendingName(){if(enabled)(void)::unlinkat(directory,name.c_str(),0);}
    } pending{ns->directory,result->name,created};
    Header header{};
    if (created) {
        std::shared_ptr<MappingWindow> image_source;
        if(protection&0x1000000U) {
            const auto fail_image=[&](std::uint32_t error)->Opened{(void)::unlinkat(ns->directory,result->name.c_str(),0);return {{},error};};
            if(!file)return fail_image(87);
            struct stat info{};if(::fstat(file->descriptor,&info) || info.st_size<=0 || static_cast<std::uint64_t>(info.st_size)>UINT32_MAX)return fail_image(193);
            auto raw=std::make_shared<MappingWindow>(static_cast<std::uint64_t>(info.st_size),MAP_PRIVATE,file->descriptor,0);
            if(raw->data==MAP_FAILED)return fail_image(os_error());
            try{result->image=parse_pe_section_image({static_cast<const std::uint8_t*>(raw->data),static_cast<std::size_t>(raw->size)},image_limit);}
            catch(const std::invalid_argument&){return fail_image(193);}
            if(protection&0x10000000U)for(auto& range:result->image->ranges)range.permissions&=~permission(Access::Execute);
            if(std::any_of(result->image->ranges.begin(),result->image->ranges.end(),[](const auto& r){return r.shared && (r.permissions&6U)==6U;}))return fail_image(50);
            size=result->image->image_size;
            image_source=std::move(raw);
        }
        if (!size || size > GuestMemory::maximum_capacity) { (void)::unlinkat(ns->directory,result->name.c_str(),0);return {{},87}; }
        header.size = (size + 4095) & ~UINT64_C(4095); header.protection = protection; header.characters = static_cast<std::uint32_t>(name.size());
        std::copy(name.begin(), name.end(), header.name.begin());
        if (::ftruncate(result->descriptor, static_cast<off_t>(data_offset + header.size)) || ::pwrite(result->descriptor, &header, sizeof(header), 0) != sizeof(header)) {
            const auto error = os_error(); (void)::unlinkat(ns->directory,result->name.c_str(),0);return {{},error};
        }
        try{set_shared_object_security(result->descriptor,security);}catch(...){(void)::unlinkat(ns->directory,result->name.c_str(),0);throw;}
        if(result->image && !write_image_section(result->descriptor,*result->image,
            {static_cast<const std::uint8_t*>(image_source->data),static_cast<std::size_t>(image_source->size)})){
            const auto error=os_error();(void)::unlinkat(ns->directory,result->name.c_str(),0);return {{},error};}
    } else {
        struct stat info{};
        if (::fstat(result->descriptor,&info) || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & 077) ||
            ::pread(result->descriptor,&header,sizeof(header),0) != sizeof(header) || header.signature != magic ||
            header.characters != name.size() || !std::equal(name.begin(),name.end(),header.name.begin()) ||
            !header.size || header.size > GuestMemory::maximum_capacity || static_cast<std::uint64_t>(info.st_size) < data_offset + header.size)
            return {{}, 6};
    }
    result->size = header.size; result->protection = header.protection;
    if((header.protection&0x1000000U) && !result->image){result->image=read_image_section(result->descriptor,header.size);if(!result->image)return {{},6};}
    mapping_detail::FileRecord record{};
    if(created && file){
        struct stat info{};
        if(file->path.size()>=record.path.size() || ::fstat(file->descriptor,&info) || !S_ISREG(info.st_mode)){(void)::unlinkat(ns->directory,result->name.c_str(),0);return {{},87};}
        record.signature=mapping_detail::file_magic;record.device=static_cast<std::uint64_t>(info.st_dev);record.inode=info.st_ino;
        std::copy(file->path.begin(),file->path.end(),record.path.begin());result->file_lifetime=file->lifetime;
    }else if(::pread(result->descriptor,&record,sizeof(record),mapping_detail::file_record_offset)!=sizeof(record))return {{},6};
    if(const auto error=attach_mapping_file(*result,record,created && file?file->descriptor:-1)){
        if(created)(void)::unlinkat(ns->directory,result->name.c_str(),0);return {{},error};
    }
    const bool file_backed=result->file_descriptor>=0 && !result->image;
    const auto map_flags=file_backed && (header.protection&0xffU)!=4 && (header.protection&0xffU)!=0x40U?MAP_PRIVATE:MAP_SHARED;
    result->window=std::make_shared<MappingWindow>(header.size,map_flags,
        file_backed?result->file_descriptor:result->descriptor,file_backed?0:data_offset);
    if (result->window->data == MAP_FAILED) {
        const auto error = os_error();if(created)(void)::unlinkat(ns->directory,result->name.c_str(),0);return {{},error};
    }
    if(created && !initial.empty()){
        if(initial.size()>header.size)return {{},87};
        std::memcpy(result->window->data,initial.data(),initial.size());
    }
    if (result->named) {
        ++header.references;
        if (::pwrite(result->descriptor,&header,sizeof(header),0) != sizeof(header)) return {{},os_error()};
        result->counted = true;
    } else (void)::unlinkat(ns->directory,result->name.c_str(),0);
    pending.enabled=false;return {std::move(result),0,!created};
}
std::shared_ptr<MemoryBacking> backing(const std::shared_ptr<Mapping>& mapping) {
    if (auto current = mapping->backing.lock()) return current;
    // 原生執行的共享標記保留軟體排序屏障，不以硬體 TSO 授權決定生命期。
    // 檔案區段可擴張，舊 backing 保留原視窗；Windows 禁止截短仍被映射的檔案，宿主端若
    // 截短，原生 MMU 視窗的 SIGBUS 由既有故障處理器轉成客體故障，不影響其他頁。先前把檔案映射標成受檢存取，
    // 只要有一個可寫的檔案 view，整個程序就掉到受檢記憶體模式並重編全部模組（Steam 的 browser／utility 各有
    // 一個 4 MiB／516 KiB 的讀寫檔案映射）。X64ARM_FILE_MAPPING_CHECKED=1 回到受檢存取以供對照。
    static const bool file_checked=[] {const auto* value=std::getenv("X64ARM_FILE_MAPPING_CHECKED");return value && std::string_view(value)=="1";}();
    const bool file_backed=mapping->file_lifetime || mapping->file_descriptor>=0;
    struct Owner {std::shared_ptr<Mapping> object;std::shared_ptr<MappingWindow> window;};
    auto owner=std::make_shared<Owner>(Owner{mapping,mapping->window});
    auto result = std::make_shared<MemoryBacking>(owner,std::span(static_cast<std::uint8_t*>(owner->window->data),static_cast<std::size_t>(owner->window->size)),
        !file_backed || !file_checked?MemoryBacking::ExternalAccess::StableShared:MemoryBacking::ExternalAccess::Checked);
    mapping->backing = result; return result;
}
bool allows(std::uint32_t protection, std::uint32_t access) {
    if(protection&0x1000000U)return !(access&~0xf003fU);
    if (!(access & 0x3fU) || (access & ~0xf003fU)) return false;
    const auto mode = protection & 0xffU;
    if((access&0x20U) && mode!=0x20U && mode!=0x40U && mode!=0x80U)return false;
    return mode == 4 || mode==0x40U || ((mode == 2 || mode == 8 || mode==0x20U || mode==0x80U) && !(access & 2));
}
#endif
}
struct FileMappingState {
#if !defined(_WIN32)
    struct Table {std::u16string generation;std::shared_ptr<Mapping> object;};
    struct Handle { std::shared_ptr<Mapping> object; std::uint32_t access; };
    struct View { std::shared_ptr<Mapping> object; std::uint64_t size{}, offset{}; bool copy{},dib{}; std::uint32_t protection{}; bool placeholder{}; };
    std::shared_ptr<Session> name_space;
    std::map<std::uint64_t,Handle> handles;
    std::map<std::uint64_t,View> views;
    std::map<std::u16string,Table> tables;
#endif
};
std::uint32_t WindowsServices::process_image_source(std::uint64_t handle,ProcessImageSource& source){
#if !defined(_WIN32)
    if(!mappings_)return 6;
    const auto found=mappings_->handles.find(handle);if(found==mappings_->handles.end())return 6;
    if(!(found->second.access&8U))return 5;
    const auto& object=found->second.object;
    if(!object->image)return 193;
    const auto& image=*object->image;
    if((object->protection&0x10000000U) || (image.characteristics&0x2000U))return 193;
    if(std::any_of(image.ranges.begin(),image.ranges.end(),[](const auto& range){return range.shared && (range.permissions&permission(Access::Write));}))return 50;
    std::array<std::uint64_t,2> marker{};struct stat info{};
    if(::pread(object->descriptor,marker.data(),sizeof(marker),image_record_offset+510*8)!=sizeof(marker) ||
       marker[0]!=image_source_magic || marker[1]!=image.file_size || !marker[1] ||
       ::fstat(object->descriptor,&info) || static_cast<std::uint64_t>(info.st_size)!=data_offset+object->size+marker[1])return 13;
    source={object->descriptor,data_offset+object->size,marker[1],static_cast<std::uint16_t>(image.machine),
        static_cast<std::uint16_t>(image.subsystem),object->path,object};
    return 0;
#else
    (void)handle;(void)source;return 50;
#endif
}
std::optional<WindowsServices::ObjectMetadata> WindowsServices::mapping_object_metadata(std::uint64_t handle)const{
#if !defined(_WIN32)
    if(!mappings_)return {};const auto at=mappings_->handles.find(handle);if(at==mappings_->handles.end())return {};const auto& object=at->second.object;
    ObjectMetadata out;out.access=at->second.access;out.identity=std::hash<std::string>{}(object->name);
    for(const auto& [h,item]:mappings_->handles){(void)h;if(item.object->name==object->name)++out.handles;}
    out.pointers=out.handles;for(const auto& [address,view]:mappings_->views){(void)address;if(view.object->name==object->name)++out.pointers;}return out;
#else
    (void)handle;return {};
#endif
}
std::optional<WindowsServices::MappingViewInfo> WindowsServices::mapping_view_info(std::uint64_t address) const {
#if !defined(_WIN32)
    if(!mappings_)return std::nullopt;
    const auto next=mappings_->views.upper_bound(address);if(next==mappings_->views.begin())return std::nullopt;
    const auto& [base,view]=*std::prev(next);
    if(address-base<view.size)return MappingViewInfo{base,view.size,view.protection};
#else
    (void)address;
#endif
    return std::nullopt;
}
bool WindowsServices::shared_mapping_page(std::uint64_t address) const {
#if !defined(_WIN32)
    if(!mappings_) return false;
    const auto next=mappings_->views.upper_bound(address);
    if(next==mappings_->views.begin()) return false;
    const auto& [base,view]=*std::prev(next);
    if(address-base>=view.size)return false;
    if(view.object->image){
        for(const auto& range:view.object->image->ranges)if(address-base>=range.rva && address-base-range.rva<range.size)return range.shared && (range.permissions&permission(Access::Write));
        return false;
    }
    return !view.copy;
#else
    (void)address;return false;
#endif
}
std::string WindowsServices::mapping_session() {
    if(!runtime_.network.external_io)throw HostCallFault("共用映射不支援 deterministic compare");
#if !defined(_WIN32)
    if (!mappings_) mappings_ = std::make_shared<FileMappingState>();
    if (!mappings_->name_space) mappings_->name_space = session();
    // session 可先於 initialize 建立或匯入；正式檔案系統建立後才完成一次掛載。
    if(!initialized_ || filesystem_.shared_session()==mappings_->name_space->path.native())return mappings_->name_space->path.string();
    const auto fonts=font_directory(*mappings_->name_space);
    if(fonts<0){const auto error=std::error_code(errno,std::generic_category());throw HostCallFault("無法準備共用字型檔案目錄："+error.message()+" (errno="+std::to_string(error.value())+")");}::close(fonts);
    filesystem_.mount_readonly("C:/Windows/Fonts/X64ARM",mappings_->name_space->path/"fonts");
    filesystem_.set_shared_session(mappings_->name_space->path.string());
    return mappings_->name_space->path.string();
#else
    return {};
#endif
}
std::shared_ptr<SharedWake> WindowsServices::mapping_wake() const {
#if !defined(_WIN32)
    return mappings_ && mappings_->name_space?mappings_->name_space->wake:nullptr;
#else
    return {};
#endif
}
int WindowsServices::mapping_session_descriptor() {
    (void)mapping_session();
#if !defined(_WIN32)
    return mappings_->name_space->lease;
#else
    return -1;
#endif
}
void WindowsServices::set_mapping_session(const std::string& path,int inherited_descriptor) {
    if(!runtime_.network.external_io)throw HostCallFault("共用映射不支援 deterministic compare");
#if !defined(_WIN32)
    if (mappings_) throw std::invalid_argument("不能替換已使用的共用映射命名空間");
    mappings_ = std::make_shared<FileMappingState>(); mappings_->name_space = session(path,inherited_descriptor);
    (void)mapping_session();
#else
    (void)path;(void)inherited_descriptor;
#endif
}
FileInformation WindowsServices::publish_font_file(std::span<const std::uint8_t> bytes) {
    if(bytes.empty() || bytes.size()>native_font_file_limit)return {{},{},87};
#if !defined(_WIN32)
    (void)mapping_session();
    struct Descriptor {int value;~Descriptor(){if(value>=0)::close(value);}};
    Descriptor directory{font_directory(*mappings_->name_space)};if(directory.value<0)return {{},{},os_error()};
    CryptoHash hash("SHA256",false,{});hash.update(bytes);std::string name;
    for(const auto byte:hash.digest()){name+="0123456789abcdef"[byte>>4];name+="0123456789abcdef"[byte&15];}name+=".ttf";
    const auto valid=[&](int fd){
        struct stat details{};if(fd<0 || ::fstat(fd,&details) || !S_ISREG(details.st_mode) || details.st_uid!=::geteuid() || (details.st_mode&0222) || static_cast<std::uint64_t>(details.st_size)!=bytes.size())return false;
        std::array<std::uint8_t,4096> buffer{};
        for(std::size_t offset=0;offset<bytes.size();){const auto size=std::min(buffer.size(),bytes.size()-offset);const auto got=::pread(fd,buffer.data(),size,static_cast<off_t>(offset));if(got<0 && errno==EINTR)continue;if(got!=static_cast<ssize_t>(size) || !std::equal(buffer.begin(),buffer.begin()+size,bytes.begin()+static_cast<std::ptrdiff_t>(offset)))return false;offset+=size;}
        return true;
    };
    Descriptor existing{::openat(directory.value,name.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW)};
    if(existing.value>=0){if(!valid(existing.value))return {{},{},5};}
    else {
        if(errno!=ENOENT)return {{},{},os_error()};
        static std::atomic<std::uint64_t> sequence{};const auto temporary="pending-"+std::to_string(::getpid())+'-'+std::to_string(sequence++);
        Descriptor output{::openat(directory.value,temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600)};
        if(output.value<0)return {{},{},os_error()};
        struct Cleanup {int directory;const std::string& name;~Cleanup(){::unlinkat(directory,name.c_str(),0);}} cleanup{directory.value,temporary};
        for(std::size_t offset=0;offset<bytes.size();){const auto written=::write(output.value,bytes.data()+offset,bytes.size()-offset);if(written<0 && errno==EINTR)continue;if(written<=0)return {{},{},os_error()};offset+=static_cast<std::size_t>(written);}
        if(::fchmod(output.value,0400))return {{},{},os_error()};
        if(::linkat(directory.value,temporary.c_str(),directory.value,name.c_str(),0)){
            if(errno!=EEXIST)return {{},{},os_error()};
            Descriptor duplicate{::openat(directory.value,name.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW)};if(!valid(duplicate.value))return {{},{},5};
        }
    }
    return filesystem_.information("C:/Windows/Fonts/X64ARM/"+name);
#else
    return {{},{},50};
#endif
}
std::size_t WindowsServices::mapping_handle_count() const {
#if !defined(_WIN32)
    return mappings_?mappings_->handles.size():0;
#else
    return 0;
#endif
}
void WindowsServices::append_mapping_handles(std::vector<std::uint64_t>& handles) const {
#if !defined(_WIN32)
    if(mappings_)for(const auto& [handle,item]:mappings_->handles){(void)item;handles.push_back(handle);}
#else
    (void)handles;
#endif
}
std::u16string WindowsServices::mapping_object_name(std::uint64_t handle) const {
#if !defined(_WIN32)
    if(mapping_handle(handle)){
        const auto& object=*mappings_->handles.at(handle).object;if(!object.named)return {};
        Header header{};if(::pread(object.descriptor,&header,sizeof(header),0)!=sizeof(header) || header.signature!=magic || header.characters>header.name.size())throw HostCallFault("Section 名稱儲存損壞");
        return {header.name.data(),header.characters};
    }
#else
    (void)handle;
#endif
    return {};
}
bool WindowsServices::mapping_handle(std::uint64_t handle) const {
#if !defined(_WIN32)
    return mappings_ && mappings_->handles.contains(handle);
#else
    (void)handle; return false;
#endif
}
std::uint32_t WindowsServices::mapping_security_location(std::uint64_t handle,std::uint32_t needed,std::vector<std::uint8_t>*& local,int& native,int& lock){
#if !defined(_WIN32)
    if(!mappings_ || !mappings_->handles.contains(handle))return 6;const auto& item=mappings_->handles.at(handle);if((item.access&needed)!=needed)return 5;
    // 檔案與 pagefile Section 均已使用共享物件紀錄；匿名檔案映射不能另讀空的本機 DACL。
    local=nullptr;native=item.object->descriptor;lock=item.object->name_space->directory;return 0;
#else
    (void)handle;(void)needed;(void)local;(void)native;(void)lock;return 50;
#endif
}
bool WindowsServices::export_mapping_handle(std::uint64_t handle,InheritedHandle& output) const {
#if !defined(_WIN32)
    if(!mappings_)return false;const auto found=mappings_->handles.find(handle);if(found==mappings_->handles.end())return false;
    output.type=2;output.access=found->second.access;output.descriptor=found->second.object->descriptor;output.lifetime=found->second.object;return true;
#else
    (void)handle;(void)output;return false;
#endif
}
void WindowsServices::import_mapping_handle(const InheritedHandle& input) {
#if !defined(_WIN32)
    (void)mapping_session();auto object=std::make_shared<Mapping>();object->descriptor=input.descriptor;object->name_space=mappings_->name_space;
    (void)::fcntl(input.descriptor,F_SETFD,FD_CLOEXEC);DirectoryLock lock(object->name_space->directory);Header header;struct stat info{};
    if(::fstat(input.descriptor,&info) || !S_ISREG(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077) || ::pread(input.descriptor,&header,sizeof(header),0)!=sizeof(header) || header.signature!=magic || header.characters>=260 || !header.size || header.size>GuestMemory::maximum_capacity || static_cast<std::uint64_t>(info.st_size)<data_offset+header.size || (input.access&~0xf001fU))throw HostCallFault("繼承的共用映射無效");
    object->size=header.size;object->protection=header.protection;object->named=header.characters!=0;
    if(header.protection&0x1000000U){object->image=read_image_section(input.descriptor,header.size);if(!object->image)throw HostCallFault("繼承的映像 Section 中繼資料無效");}
    object->name=object_name(std::u16string_view(header.name.data(),header.characters));
    mapping_detail::FileRecord record{};
    if(::pread(input.descriptor,&record,sizeof(record),mapping_detail::file_record_offset)!=sizeof(record) || attach_mapping_file(*object,record))throw HostCallFault("無法匯入檔案映射描述符");
    const bool file_backed=object->file_descriptor>=0 && !object->image;
    const auto flags=file_backed && (header.protection&0xffU)!=4 && (header.protection&0xffU)!=0x40U?MAP_PRIVATE:MAP_SHARED;
    object->window=std::make_shared<MappingWindow>(header.size,flags,file_backed?object->file_descriptor:input.descriptor,file_backed?0:data_offset);
    if(object->window->data==MAP_FAILED)throw HostCallFault("無法匯入繼承的共用映射");
    if(object->named){++header.references;if(::pwrite(input.descriptor,&header,sizeof(header),0)!=sizeof(header))throw HostCallFault("無法更新繼承映射引用");object->counted=true;}
    mappings_->handles.emplace(input.handle,FileMappingState::Handle{std::move(object),input.access});
#else
    (void)input;throw HostCallFault("此平台不支援繼承映射");
#endif
}
bool WindowsServices::close_mapping(std::uint64_t handle) {
#if !defined(_WIN32)
    if (!mappings_) return false;
    return mappings_->handles.erase(handle) != 0;
#else
    (void)handle; return false;
#endif
}
std::uint64_t WindowsServices::map_shared_table(GuestMemory& memory,std::u16string_view family,std::u16string_view generation,std::span<const std::uint8_t> bytes,bool writable,const std::function<void()>& publish,std::u16string_view previous_generation){
#if defined(_WIN32)
    (void)memory;(void)family;(void)generation;(void)bytes;(void)writable;(void)publish;(void)previous_generation;last_error_=50;return 0;
#else
    (void)mapping_session();
    if(family.empty() || generation.empty() || family.size()+generation.size()+1>=260 || mappings_->handles.size()>=4096){last_error_=87;return 0;}
    if(!mappings_->tables.contains(std::u16string(family)) && mappings_->tables.size()>=64){last_error_=8;return 0;}
    auto& slot=mappings_->tables[std::u16string(family)];
    auto object=slot.generation==generation?slot.object:std::shared_ptr<Mapping>{};
    bool created=false;
    if(!object){
        const auto name=std::u16string(family)+u":"+std::u16string(generation);
        const auto security=synchronization_descriptor(memory,0);
        auto opened=open_mapping(mappings_->name_space,name,bytes.size(),4,!bytes.empty(),security.bytes,nullptr,
                                 GuestMemory::capacity_limit,bytes,true);
        if(opened.error){last_error_=opened.error;return 0;}
        created=!opened.existed;object=std::move(opened.mapping);
    }
    struct Rollback {std::shared_ptr<Mapping> object;bool pending;~Rollback(){if(pending)object->persistent=false;}} rollback{object,created};
    FileMappingState::Table next{std::u16string(generation),object};
    const auto handle=runtime_.allocate_wait_handle();mappings_->handles.emplace(handle,FileMappingState::Handle{object,0xf001fU});
    struct Close {FileMappingState& state;std::uint64_t handle;~Close(){state.handles.erase(handle);}} close{*mappings_,handle};
    const auto address=map_file_view(memory,handle,writable?2U:1U,0,0);
    if(address){
        try{if(publish)publish();}catch(...){(void)unmap_file_view(memory,address);throw;}
        std::swap(slot,next); // 舊 view 持有舊區段；新的 table 不覆寫它。
        rollback.pending=false;
        if(!previous_generation.empty() && previous_generation!=generation){
            const auto retired="table-"+object_name(std::u16string(family)+u":"+std::u16string(previous_generation));
            DirectoryLock lock(mappings_->name_space->directory);(void)::unlinkat(mappings_->name_space->directory,retired.c_str(),0);
        }
    }
    return address;
#endif
}
std::uint64_t WindowsServices::map_file_view(GuestMemory& memory,std::uint64_t id,std::uint32_t access,std::uint64_t offset,std::uint64_t size,std::uint64_t address,bool dib,bool copy_override,bool replace_placeholder,const VirtualAddressRequirements* requirements){
    const auto fail=[&](std::uint32_t error){last_error_=error;return UINT64_C(0);};
#if defined(_WIN32)
    (void)memory;(void)id;(void)access;(void)offset;(void)size;(void)address;(void)dib;(void)copy_override;(void)replace_placeholder;(void)requirements;return fail(50);
#else
    if(!runtime_.network.external_io)throw HostCallFault("共用映射不支援 deterministic compare");
    if(!mappings_)return fail(6);const auto handle=mappings_->handles.find(id);
    if(handle==mappings_->handles.end())return fail(6);const auto alignment=replace_placeholder?UINT64_C(4096):granularity;if(offset%alignment)return fail(1132);
    if(!allows(handle->second.object->protection,access) || ((access&0x1fU)&~handle->second.access))return fail(5);
    if((access&0x20U) && !(handle->second.access&0x28U))return fail(5);
    auto object=handle->second.object;if(const auto error=refresh_mapping_size(*object))return fail(error);
    if(object->image && (offset || replace_placeholder || dib))return fail(50);
    if(offset>=object->size || size>object->size-offset)return fail(87);
    if(object->image)size=object->size;
    if(!size)size=object->size-offset;size=(size+4095)&~UINT64_C(4095);const bool fixed=address!=0;
    const auto limit=user_address_limit();
    const auto align=requirements && requirements->alignment?std::max(alignment,requirements->alignment):alignment;
    const auto low=requirements?std::max(granularity,requirements->lowest):granularity;
    const auto high=requirements && requirements->highest?std::min(limit-1,requirements->highest):limit-1;
    if((align&(align-1)) || low>high || size>high-low+1 || (fixed && requirements && (requirements->lowest || requirements->highest || requirements->alignment)))return fail(87);
    const auto align_up=[&](std::uint64_t value){return value>UINT64_MAX-(align-1)?UINT64_MAX:(value+align-1)&~(align-1);};
    auto placeholder=runtime_.virtual_regions.end();
    if(replace_placeholder){
        placeholder=runtime_.virtual_regions.find(address);
        if(!fixed || dib || placeholder==runtime_.virtual_regions.end() || !placeholder->second.placeholder || placeholder->second.size!=size)return fail(487);
    }
    if(!address && object->image){
        const auto preferred=object->image->preferred_base;
        if(preferred>=low && preferred<=high && size<=high+1-preferred && !(preferred%align))address=preferred;
    }
    if(!address)address=align_up(requirements?low:runtime_.next_virtual);
    for(;;){
        if(address%align || address<low || address>high || size>high+1-address)return fail(487);
        const auto end=address+size;std::uint64_t conflict_end=0;
        // MEM_RESERVE 的空頁仍佔用虛擬位址；section view 不能覆蓋私人保留區。
        const auto next=runtime_.virtual_regions.upper_bound(address);
        if(next!=runtime_.virtual_regions.begin()){
            const auto previous=std::prev(next);
            if(previous!=placeholder && previous->first+previous->second.size>address)conflict_end=previous->first+previous->second.size;
        }
        if(next!=runtime_.virtual_regions.end() && next->first<end)conflict_end=std::max(conflict_end,next->first+next->second.size);
        auto mapped=memory.query(GuestAddress{address});if(!mapped)mapped=memory.next_mapping(GuestAddress{address});
        if(mapped && mapped->base.value<end)conflict_end=std::max(conflict_end,mapped->base.value+mapped->size);
        if(!conflict_end)break;
        if(fixed)return fail(487);
        address=align_up(conflict_end);
    }
    if(object->image){
        std::uint64_t needed=0;for(const auto& range:object->image->ranges)needed+=range.size;
        if(needed>memory.remaining_capacity() || runtime_.virtual_regions.size()>=65536)return fail(8);
        bool reservation=false,region=false,record=false;
        try{
            const auto mode=0x1000000U|0x80U;
            const auto inserted=mappings_->views.emplace(address,FileMappingState::View{object,size,0,true,false,mode,false});
            if(!inserted.second)return fail(487);record=true;
            const auto reserved=runtime_.virtual_regions.emplace(address,VirtualRegion{size,0x80U,false,false});
            if(!reserved.second){mappings_->views.erase(address);return fail(487);}region=true;
            memory.reserve_virtual_range(GuestAddress{address},size);reservation=true;
            for(const auto& range:object->image->ranges){
                const GuestAddress at{address+range.rva};const bool shared=range.shared && (range.permissions&permission(Access::Write));
                if(shared)memory.map_shared(at,range.size,range.permissions,backing(object),range.rva);
                else{
                    const auto* data=static_cast<const std::uint8_t*>(object->window->data)+range.rva;
                    memory.map(at,range.size,range.permissions,{data,range.size});
                    if(range.permissions&permission(Access::Write))memory.make_copy_on_write(at,range.size,range.permissions);
                }
            }
            if(!fixed)runtime_.next_virtual=(address+size+granularity-1)&~(granularity-1);
            return address;
        }catch(...){
            if(reservation){memory.discard(GuestAddress{address},size);memory.release_virtual_range(GuestAddress{address},size);}
            if(region)runtime_.virtual_regions.erase(address);if(record)mappings_->views.erase(address);
            try{throw;}catch(const std::bad_alloc&){return fail(8);}catch(const MemoryFault&){return fail(487);}catch(const std::invalid_argument&){return fail(487);}
        }
    }
    if(size>memory.remaining_capacity())return fail(8);
    if(access&0x20U)return fail(50); // 外部共享可執行頁尚無跨程序程式碼失效協定。
    try {
        const bool copy=(access&3U)==1 || copy_override;const auto permissions=static_cast<Permissions>((copy || (access&3U)?read_write:permission(Access::Read))|((access&0x20U)?permission(Access::Execute):0));
        // 先配置記錄；map_shared 失敗時回滾，不遺留無法 Unmap 的映射。
        const auto mode=copy?8U:(access&2U)?4U:2U;
        const auto inserted=mappings_->views.emplace(address,FileMappingState::View{object,size,offset,copy,dib,(access&0x20U)?mode<<4:mode,replace_placeholder});if(!inserted.second)return fail(487);
        try{
            if(copy){memory.map_shared(GuestAddress{address},static_cast<std::size_t>(size),permission(Access::Read),backing(object),static_cast<std::size_t>(offset));try{memory.make_copy_on_write(GuestAddress{address},static_cast<std::size_t>(size),permissions);}catch(...){memory.discard(GuestAddress{address},static_cast<std::size_t>(size));throw;}}
            else memory.map_shared(GuestAddress{address},static_cast<std::size_t>(size),permissions,backing(object),static_cast<std::size_t>(offset));
        }catch(...){mappings_->views.erase(inserted.first);throw;}
        if(replace_placeholder){placeholder->second.placeholder=false;placeholder->second.replaced_placeholder=true;placeholder->second.protection=mode;}
        if(!fixed)runtime_.next_virtual=(address+size+granularity-1)&~(granularity-1);return address;
    }catch(const std::bad_alloc&){return fail(8);}
#endif
}
bool WindowsServices::unmap_file_view(GuestMemory& memory,std::uint64_t address,bool dib,std::uint32_t flags){
#if defined(_WIN32)
    (void)memory;(void)address;(void)dib;(void)flags;last_error_=50;return false;
#else
    if(flags&~3U){last_error_=87;return false;}
    if(!mappings_){last_error_=487;return false;}const auto view=mappings_->views.find(address);
    if(view==mappings_->views.end() || view->second.dib!=dib){last_error_=487;return false;}
    if((flags&2U) && !view->second.placeholder){last_error_=87;return false;}
    if(flags&1U){
        // 映射仍存在時向宿主提出短期重用提示；此提示不改變內容或持久化語義。
        auto& window=*view->second.object->window;(void)::madvise(window.data,static_cast<std::size_t>(window.size),MADV_WILLNEED);
    }
    memory.discard(GuestAddress{address},static_cast<std::size_t>(view->second.size));
    if(view->second.object->image){memory.release_virtual_range(GuestAddress{address},view->second.size);runtime_.virtual_regions.erase(address);}
    if(view->second.placeholder){
        if(flags&2U)runtime_.virtual_regions.at(address)=VirtualRegion{view->second.size,1,true,false};
        else {memory.release_virtual_range(GuestAddress{address},view->second.size);runtime_.virtual_regions.erase(address);}
    }
    runtime_.virtual_page_protections.erase(runtime_.virtual_page_protections.lower_bound(address),runtime_.virtual_page_protections.lower_bound(address+view->second.size));mappings_->views.erase(view);return true;
#endif
}
bool WindowsServices::flush_file_view(std::uint64_t address,std::uint64_t length){
#if defined(_WIN32)
    (void)address;(void)length;last_error_=50;return false;
#else
    const auto fail=[&](std::uint32_t error){last_error_=error;return false;};
    if(!mappings_)return fail(487);
    auto view=mappings_->views.upper_bound(address);if(view==mappings_->views.begin())return fail(487);
    --view;const auto offset=address-view->first;
    if(offset>=view->second.size || (length && length>view->second.size-offset))return fail(487);
    if(view->second.copy)return true;
    if(::msync(view->second.object->window->data,static_cast<std::size_t>(view->second.object->window->size),MS_SYNC))return fail(os_error());
    return true;
#endif
}
std::uint64_t WindowsServices::map_dib_section(GuestMemory& memory,std::uint64_t handle,std::uint32_t offset,std::uint64_t size,std::uint64_t& view){
    view=0;
#if defined(_WIN32)
    (void)memory;(void)handle;(void)offset;(void)size;last_error_=50;return 0;
#else
    if(offset%4){last_error_=87;return 0;}if(!mappings_ || !mappings_->handles.contains(handle)){last_error_=6;return 0;}
    const auto protection=mappings_->handles.at(handle).object->protection;if(protection!=4 && protection!=8){last_error_=87;return 0;}
    const auto base=static_cast<std::uint64_t>(offset)&~(granularity-1),delta=offset-base;
    view=map_file_view(memory,handle,protection==8?1U:2U,base,delta+size,0,true);return view?view+delta:0;
#endif
}
std::span<const WindowsServiceDescriptor> mapping_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"psapi.dll","GetMappedFileNameA",4},{"psapi.dll","GetMappedFileNameW",4},{"kernel32.dll","K32GetMappedFileNameA",4},{"kernel32.dll","K32GetMappedFileNameW",4},{"ntdll.dll","NtQuerySection",5},{"kernel32.dll","CreateFileMappingW",6},{"kernel32.dll","CreateFileMappingA",6},
        {"kernel32.dll","OpenFileMappingW",3},{"kernel32.dll","OpenFileMappingA",3},
        {"kernel32.dll","MapViewOfFile",5},{"kernel32.dll","MapViewOfFileEx",6},
        {"kernel32.dll","UnmapViewOfFile",1},{"kernel32.dll","FlushViewOfFile",2}
    };return table;
}
std::uint32_t WindowsServices::native_section_access(std::uint64_t handle,std::uint32_t access){
#if defined(_WIN32)
    (void)handle;(void)access;return 0xc00000bbU;
#else
    if(!mappings_)return 0xc0000008U;const auto found=mappings_->handles.find(handle);if(found==mappings_->handles.end())return 0xc0000008U;
    if(access&~found->second.access)return 0xc0000022U;found->second.access=access;return 0;
#endif
}
std::optional<std::uint32_t> WindowsServices::invoke_nt_section(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto saved=last_error_;
    const auto finish=[&](std::uint32_t status){threads_.items.at(threads_.current).last_native_status=status;last_error_=saved;arg.return_to(state,status);return std::optional<std::uint32_t>{};};
#if defined(_WIN32)
    return finish(0xc00000bbU);
#else
    try{
        if(service.symbol=="NtQuerySection" || service.symbol=="NtExtendSection"){
            if(!mappings_ || !mappings_->handles.contains(arg(0)))return finish(object_type(arg(0)).empty()?0xc0000008U:0xc0000024U);
            const auto& handle=mappings_->handles.at(arg(0));const bool extend=service.symbol=="NtExtendSection";
            if(!(handle.access&(extend?16U:1U)))return finish(0xc0000022U);
            auto& object=*handle.object;
            if(!extend){
                if(arg(1)>1)return finish(0xc0000003U);
                if(arg(1)==1 && !object.image)return finish(0xc0000049U);
                const auto size=arg(1)==1?(x86_?48U:64U):(x86_?16U:24U);
                if(arg(4))memory.store(GuestAddress{arg(4)},x86_?32:64,size);
                if(arg(3)<size)return finish(0xc0000004U);
                memory.check(GuestAddress{arg(2)},size,Access::Write);
                if(const auto error=refresh_mapping_size(object))return finish(error==8?0xc0000017U:0xc0000185U);
                if(arg(1)==1){memory.write(GuestAddress{arg(2)},image_section_information(*object.image,x86_));return finish(0);}
                memory.write(GuestAddress{arg(2)},std::vector<std::uint8_t>(size));
                memory.store(GuestAddress{arg(2)+(x86_?4:8)},32,object.image?((object.protection&~0xffU)|0x800000U):object.file_descriptor>=0?0x8800000U:0x8000000U);
                memory.store(GuestAddress{arg(2)+(x86_?8:16)},64,object.size);return finish(0);
            }
            memory.check(GuestAddress{arg(1)},8,Access::Write);const auto requested=memory.load(GuestAddress{arg(1)},64);
            if(!requested || requested>INT64_MAX)return finish(0xc000000dU);
            if(object.image)return finish(0xc0000087U);
            if(requested>GuestMemory::maximum_capacity)return finish(0xc0000040U);
            if(object.file_descriptor<0)return finish(0xc00000f1U);
            DirectoryLock lock(object.name_space->directory);Header header{};
            if(::pread(object.descriptor,&header,sizeof(header),0)!=sizeof(header) || header.signature!=magic)return finish(0xc0000185U);
            const auto size=(requested+4095)&~UINT64_C(4095);
            if(size>header.size){
                struct stat info{};if(::fstat(object.file_descriptor,&info))return finish(0xc0000185U);
                auto window=std::make_shared<MappingWindow>(size,mapping_window_flags(object),object.file_descriptor,0);
                if(window->data==MAP_FAILED)return finish(0xc0000017U);
                if(static_cast<std::uint64_t>(info.st_size)<size && ::ftruncate(object.file_descriptor,static_cast<off_t>(size)))return finish(0xc00000f1U);
                // 中繼資料發布受既有跨程序鎖保護；舊 view 不擴大、不換址。
                if(::ftruncate(object.descriptor,static_cast<off_t>(data_offset+size)))return finish(0xc00000f1U);
                header.size=size;if(::pwrite(object.descriptor,&header,sizeof(header),0)!=sizeof(header))return finish(0xc0000185U);
                object.window=std::move(window);object.size=size;object.backing.reset();
            }else if(const auto error=refresh_mapping_window(object,header))return finish(error==8?0xc0000017U:0xc0000185U);
            memory.store(GuestAddress{arg(1)},64,object.size);return finish(0);
        }
        if(service.symbol=="NtFlushVirtualMemory"){
            if(!current_process_handle(arg(0),8U))return finish(current_process_handle(arg(0))?0xc0000022U:process_object_type(arg(0))==u"Process"?0xc00000bbU:0xc0000008U);
            const auto unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
            memory.check(GuestAddress{arg(1)},unit,Access::Write);memory.check(GuestAddress{arg(2)},unit,Access::Write);
            if(arg(3))memory.check(GuestAddress{arg(3)},unit*2,Access::Write);
            const auto address=memory.load(GuestAddress{arg(1)},bits),length=memory.load(GuestAddress{arg(2)},bits);
            const auto view=mapping_view_info(address);std::uint32_t status=0;
            if(!view || (length && length>view->size-(address-view->base)))status=0xc0000019U;
            else if(!flush_file_view(address,length))status=last_error_==8?0xc0000017U:last_error_==5?0xc0000022U:last_error_==1117?0xc0000185U:0xc000000dU;
            else{
                const auto begin=address&~UINT64_C(4095),count=length?length:view->size-(address-view->base);
                memory.store(GuestAddress{arg(1)},bits,begin);memory.store(GuestAddress{arg(2)},bits,(address-begin+count+4095)&~UINT64_C(4095));
            }
            if(arg(3)){memory.store(GuestAddress{arg(3)},bits,status);memory.store(GuestAddress{arg(3)+unit},bits,0);}
            return finish(status);
        }
        if(!current_process_handle(arg(1),8U))return finish(current_process_handle(arg(1))?0xc0000022U:process_object_type(arg(1))==u"Process"?0xc00000bbU:0xc0000008U);
        if(!mappings_)return finish(0xc0000008U);const auto found=mappings_->handles.find(arg(0));if(found==mappings_->handles.end())return finish(0xc0000008U);
        const bool extended=service.symbol=="NtMapViewOfSectionEx";const auto allocation=static_cast<std::uint32_t>(arg(extended?5:8));
        if(!extended && (arg(3) || (arg(7)!=1 && arg(7)!=2)))return finish(0xc00000bbU);
        if(allocation && allocation!=0x4000U)return finish((allocation&~0x20006000U)?0xc000000dU:0xc00000bbU);
        const bool replace=allocation==0x4000U;
        const auto unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
        const auto size_pointer=arg(extended?4:6),offset_pointer=arg(extended?3:5);
        memory.check(GuestAddress{arg(2)},unit,Access::Write);memory.check(GuestAddress{size_pointer},unit,Access::Write);
        if(offset_pointer)memory.check(GuestAddress{offset_pointer},8,extended?Access::Read:Access::Write);
        const auto base=memory.load(GuestAddress{arg(2)},bits),length=memory.load(GuestAddress{size_pointer},bits),offset=offset_pointer?memory.load(GuestAddress{offset_pointer},64):0;
        const bool image=found->second.object->image.has_value();
        const auto protection=static_cast<std::uint32_t>(arg(extended?6:9));const auto mode=protection&0xffU;const bool copy=mode==8 || mode==0x80U,execute=mode==0x10U || mode==0x20U || mode==0x40U || mode==0x80U;
        if(mode!=2 && mode!=4 && mode!=8 && mode!=0x20U && mode!=0x40U && mode!=0x80U && !(image && (mode==1 || mode==0x10U)))return finish(0xc0000045U);
        if(image && (found->second.object->protection&0x10000000U) && mode!=2)return finish(0xc0000045U);
        if(const auto error=refresh_mapping_size(*found->second.object))return finish(error==8?0xc0000017U:0xc0000185U);
        const auto alignment=replace?UINT64_C(4096):granularity;
        if(protection!=mode || offset>INT64_MAX || offset%alignment || base%alignment || offset>=found->second.object->size || length>found->second.object->size-offset || (replace && (!base || !length || (length&4095U))))return finish(0xc000000dU);
        if(image && (offset || replace))return finish(0xc00000bbU);
        const auto size=image?found->second.object->size:length?(length+4095)&~UINT64_C(4095):found->second.object->size-offset;if(!extended && arg(4)>size)return finish(0xc000000dU);
        VirtualAddressRequirements requirements;const VirtualAddressRequirements* constraints=nullptr;
        if(extended){
            const auto array=arg(7);const auto count=static_cast<std::uint32_t>(arg(8));if(count>32 || (count && !array))return finish(0xc000000dU);
            if(count)memory.check(GuestAddress{array},static_cast<std::size_t>(count)*16,Access::Read);std::uint64_t used=0;
            for(unsigned i=0;i<count;++i){
                const auto type=memory.load(GuestAddress{array+i*16},64),value=memory.load(GuestAddress{array+i*16+8},64);
                if(!type || type>6 || (used&(UINT64_C(1)<<type)))return finish(0xc000000dU);used|=UINT64_C(1)<<type;
                if(type==1){
                    if(!value || (x86_ && value>UINT32_MAX))return finish(0xc000000dU);memory.check(GuestAddress{value},unit*3,Access::Read);
                    requirements={memory.load(GuestAddress{value},bits),memory.load(GuestAddress{value+unit},bits),memory.load(GuestAddress{value+2*unit},bits)};constraints=&requirements;
                }else if(type==2){if(value)return finish(0xc000000dU);}
                else return finish(0xc00000bbU);
            }
        }
        const auto access=(mode==4 || mode==0x40U?2U:4U)|(execute?0x20U:0U);const auto view=map_file_view(memory,arg(0),access,offset,size,base,false,copy,replace,constraints);
        if(!view)return finish(last_error_==5?0xc0000022U:last_error_==8?0xc0000017U:last_error_==487?0xc0000018U:last_error_==6?0xc0000008U:last_error_==50?0xc00000bbU:0xc000000dU);
        memory.store(GuestAddress{arg(2)},bits,view);memory.store(GuestAddress{size_pointer},bits,size);if(!extended && offset_pointer)memory.store(GuestAddress{offset_pointer},64,offset);
        return finish(image && view!=found->second.object->image->preferred_base?0x40000003U:0);
    }catch(const MemoryFault&){return finish(0xc0000005U);}catch(const std::bad_alloc&){return finish(0xc0000017U);}
#endif
}
std::optional<std::uint32_t> WindowsServices::invoke_mapping(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    if(!runtime_.network.external_io)throw HostCallFault("共用映射不支援 deterministic compare");
    if(service.symbol=="NtQuerySection")return invoke_nt_section(state,memory,service);
    const WindowsCall arg(state,memory,service); const auto symbol = service.symbol;
    const auto dword = [&](unsigned index) { return static_cast<std::uint32_t>(arg(index)); };
    std::uint64_t result = 0;
    const auto fail = [&](std::uint32_t error) { last_error_ = error; result = 0; };
#if defined(_WIN32)
    fail(50);
#else
    try{
    if(service.family!="internal-nt-mapping")if(const auto native=invoke_native_mapping(state,memory,service)){
        arg.return_to(state,*native);return std::nullopt;
    }
    (void)mapping_session();
    if(symbol.find("GetMappedFileName")!=symbol.npos){
        if(!current_process_handle(arg(0),0x400))fail(6);
        else if(!arg(2) || !dword(3))fail(87);
        else {
            std::u16string path;
            if(const auto* image=module_image(arg(1),true))path=image->path;
            else {auto view=mappings_->views.upper_bound(arg(1));if(view!=mappings_->views.begin()){--view;if(arg(1)-view->first<view->second.size)path=view->second.object->path;}}
            if(path.empty())fail(memory.query(GuestAddress{arg(1)})?1006:487);
            else {std::replace(path.begin(),path.end(),u'/',u'\\');if(path.size()<2 || path[1]!=u':')path=normalized_windows_path(path,runtime_.current_directory);
                path=windows_device_name(path[0])+path.substr(2);
                const bool wide=symbol.ends_with('W');const auto ansi=utf16_to_utf8(path);const auto length=wide?path.size():ansi.size(),count=std::min<std::size_t>(length,dword(3)-1);
                if(wide)memory.write(GuestAddress{arg(2)},utf16_bytes(path.substr(0,count)));else {std::vector<std::uint8_t> bytes(ansi.begin(),ansi.begin()+static_cast<std::ptrdiff_t>(count));bytes.push_back(0);memory.write(GuestAddress{arg(2)},bytes);}
                result=std::min<std::uint64_t>(length,dword(3));if(length>=dword(3))last_error_=122;
            }
        }
    }
    else if(symbol=="DuplicateHandle"){
        const auto source=mappings_->handles.find(arg(1));const bool close_source=(dword(6)&1U)!=0;
        const bool current=current_process_handle(arg(0),0x40),protected_source=(runtime_.handle_flags[arg(1)]&2U)!=0;
        const auto close=[&]{if(close_source && current && !protected_source){mappings_->handles.erase(arg(1));runtime_.handle_flags.erase(arg(1));}};
        struct Cleanup {const decltype(close)& action;~Cleanup(){action();}} cleanup{close};
        if(source==mappings_->handles.end() || !current || (!current_process_handle(arg(2),0x40) && !(close_source && !arg(2))))fail(6);
        else if(dword(6)&~3U)fail(87);
        else if(close_source && protected_source)fail(5);
        else if(!arg(2))result=1;
        else if(!(dword(6)&2U) && (dword(4)&~source->second.access))fail(5);
        else if(mappings_->handles.size()>=4096)fail(8);
        else {
            if(arg(3))memory.check(GuestAddress{arg(3)},x86_?4:8,Access::Write);
            const auto token=runtime_.allocate_wait_handle();auto copy=source->second;if(!(dword(6)&2U))copy.access=dword(4);
            mappings_->handles.emplace(token,std::move(copy));runtime_.handle_flags[token]=arg(5)?1:0;
            if(arg(3))memory.store(GuestAddress{arg(3)},x86_?32:64,token);result=1;
        }
    }
    else if (symbol.starts_with("CreateFileMapping") || symbol.starts_with("OpenFileMapping")) {
        const bool create = symbol.starts_with("Create"); const auto name_at = arg(create ? 5 : 2);
        // Native 建立／再次開啟保留原 DesiredAccess；不能為共用 Win32 流程多要求權限，
        // 再於完成時縮小 handle。新建與既有物件均沿相同權限值，DACL 仍照原流程檢查。
        const auto create_access=create && service.family=="internal-nt-mapping"?dword(6):0xf001fU;
        const auto security=create?synchronization_descriptor(memory,arg(1)):SynchronizationDescriptor{arg(1)!=0,{}};
        auto name = name_at ? (symbol.ends_with('W') ? read_utf16(memory,GuestAddress{name_at}) : utf8_to_utf16(read_ansi(memory,name_at))) : std::u16string{};
        if(name.starts_with(u"Local\\"))name.erase(0,6);
        const auto protection = create ? dword(2) : 0;
        const auto attributes=protection&~0xffU,mode=protection&0xffU;
        const bool image=(attributes&0x1000000U)!=0;
        const auto size = create ? (std::uint64_t{dword(3)} << 32) | dword(4) : 0;
        if (create && ((attributes && attributes!=0x8000000U && attributes!=0x1000000U && attributes!=0x11000000U) ||
            (mode!=2 && mode!=4 && mode!=8 && mode!=0x20U && mode!=0x40U && mode!=0x80U) ||
            (attributes==0x11000000U && mode!=2))) fail(87);
        else if (auto existing=create && !name.empty()?open_mapping(mappings_->name_space,name,0,0,false,{}):Opened{};existing.mapping){
            // 已存在時沿用原物件；不能先擴張呼叫者此次傳入的檔案。
            DirectoryLock lock(mappings_->name_space->directory);
            const auto descriptor=shared_object_security(existing.mapping->descriptor);
            const auto granted=object_security_access(descriptor,create_access,{0x20005U,0x20012U,0x20008U,0xf001fU});
            result=runtime_.allocate_wait_handle();mappings_->handles.emplace(result,FileMappingState::Handle{std::move(existing.mapping),granted});runtime_.handle_flags[result]=security.inherit?1U:0U;last_error_=183;
        }
        else if (create && arg(0) != (x86_?UINT32_MAX:UINT64_MAX)) {
            const auto file = files_.find(arg(0));
            if (file == files_.end()) fail(6);
            else if (file->second.native && file->second.native->null_device()) fail(6);
            else if (!file->second.native) fail(50);
            else if (!(file->second.access & (0x80000000U|1U|0x20U)) || (!image && (mode==4 || mode==0x40U) && !(file->second.access & (0x40000000U|2U)))) fail(5);
            else {
                auto info = file->second.native->information(); const auto bytes = image?info.metadata.size:size?size:info.metadata.size;
                if (info.error) fail(info.error);
                else if (!bytes || bytes > (image?UINT32_MAX:GuestMemory::maximum_capacity)) fail(image?193:bytes?87:1006);
                else if (bytes > info.metadata.size && !(file->second.access & (0x40000000U|2U))) fail(5);
                else if (bytes > info.metadata.size && file->second.native->truncate(bytes)) fail(5);
                else {
                    mapping_detail::Fd native{file->second.native->duplicate_descriptor()};
                    if(native.value<0)fail(os_error());
                    else {
                        // 匿名與命名檔案 section 共用中繼資料及檔案 broker，擴張和繼承
                        // 不另走持有裸 fd、無法發布新大小的特例。
                        const MappingFileInput input{native.value,utf8_to_utf16(file->second.path),file->second.native->retain_file()};
                        auto opened=open_mapping(mappings_->name_space,name,bytes,protection,true,security.bytes,&input,memory.limit());
                        if(opened.error)fail(opened.error);
                        else {
                            const auto granted=opened.existed?object_security_access(shared_object_security(opened.mapping->descriptor),create_access,{0x20005U,0x20012U,0x20008U,0xf001fU}):create_access;
                            result=runtime_.allocate_wait_handle();mappings_->handles.emplace(result,FileMappingState::Handle{std::move(opened.mapping),granted});runtime_.handle_flags[result]=security.inherit?1U:0U;last_error_=opened.existed?183U:0U;
                        }
                    }
                }
            }
        }
        else if (!create && name.empty()) fail(87);
        else {
            auto opened = open_mapping(mappings_->name_space,name,size,protection,create,security.bytes);
            if (opened.error) fail(opened.error);
            else {
                const auto access = create ? create_access : dword(0);
                if (!create && !allows(opened.mapping->protection,access)) fail(5);
                else {
                    DirectoryLock lock(mappings_->name_space->directory);const auto descriptor=shared_object_security(opened.mapping->descriptor);if(descriptor.empty())throw SecurityDescriptorError{1338};// 新建物件的初始 handle 具完整權限；DACL 保護之後的開啟。
                    const auto granted=create && !opened.existed?access:object_security_access(descriptor,access,{0x20005U,0x20012U,0x20008U,0xf001fU});
                    result = runtime_.allocate_wait_handle(); mappings_->handles.emplace(result,FileMappingState::Handle{std::move(opened.mapping),granted});runtime_.handle_flags[result]=security.inherit?1U:0U;
                    if(create) last_error_ = opened.existed ? 183 : 0;
                }
            }
        }
    } else if (symbol == "MapViewOfFile" || symbol == "MapViewOfFileEx") {
        result=map_file_view(memory,arg(0),dword(1),(std::uint64_t{dword(2)}<<32)|dword(3),arg(4),symbol=="MapViewOfFileEx"?arg(5):0);
    } else if (symbol == "UnmapViewOfFile") {
        result=unmap_file_view(memory,arg(0));
    } else if (symbol == "FlushViewOfFile") {
        result=flush_file_view(arg(0),arg(1));
    } else throw HostCallFault("未知 FileMapping 服務");
    }catch(const SecurityDescriptorError& error){fail(error.code);}catch(const MemoryFault&){fail(998);}catch(const std::bad_alloc&){fail(8);}catch(const std::invalid_argument&){fail(123);}
#endif
    arg.return_to(state,result);return std::nullopt;
}
std::uint64_t WindowsServices::map_host_memory(GuestMemory& memory,std::shared_ptr<void> owner,std::span<std::uint8_t> bytes,bool checked){
    // 與檔案檢視相同的位址搜尋：避開保留區與既有映射，以配置粒度對齊。
    const auto limit=user_address_limit();const auto size=bytes.size();
    std::uint64_t address=(runtime_.next_virtual+granularity-1)&~(granularity-1);
    for(;;){
        if(address<granularity || address>=limit || size>limit-address)return 0;
        const auto end=address+size;std::uint64_t conflict_end=0;
        const auto next=runtime_.virtual_regions.upper_bound(address);
        if(next!=runtime_.virtual_regions.begin()){const auto previous=std::prev(next);if(previous->first+previous->second.size>address)conflict_end=previous->first+previous->second.size;}
        if(next!=runtime_.virtual_regions.end() && next->first<end)conflict_end=std::max(conflict_end,next->first+next->second.size);
        auto mapped=memory.query(GuestAddress{address});if(!mapped)mapped=memory.next_mapping(GuestAddress{address});
        if(mapped && mapped->base.value<end)conflict_end=std::max(conflict_end,mapped->base.value+mapped->size);
        if(!conflict_end)break;
        address=(conflict_end+granularity-1)&~(granularity-1);
    }
    if(size>memory.remaining_capacity())return 0;
    try{
        // 生命週期由 owner 持有（撤除客體映射前不釋放），長度固定：StableShared 讓客體存取不經受檢慢速路徑。
        auto backing=std::make_shared<MemoryBacking>(std::move(owner),bytes,checked?MemoryBacking::ExternalAccess::Checked:MemoryBacking::ExternalAccess::StableShared);
        memory.map_shared(GuestAddress{address},size,read_write,backing,0);
    }catch(const std::exception&){return 0;}
    runtime_.next_virtual=(address+size+granularity-1)&~(granularity-1);
    return address;
}
}
