#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "service_callback_task.hpp"
#include <algorithm>

// shell32 磁碟、路徑與回收區家族：DriveType／RealDriveType／IsNetDrive、IsLFNDrive、PathCleanupSpec、PathGetShortPath、PathIsExe、
// PathMakeUniqueName／PathYetAnotherMakeUniqueName、PathQualify、PathResolve、SHPathPrepareForWrite、SHValidateUNC、CheckEscapes、
// SheChangeDir／SheGetDir、SHQueryRecycleBin／SHEmptyRecycleBin／SHUpdateRecycleBinIcon。
// 這些 shell32 舊介面在 NT 上為 Unicode；有 A／W 版本者 A 版依 ACP（UTF-8）轉換。檔案系統操作交給既有 kernel32 服務，
// 回收區沿用 SHFileOperation(FOF_ALLOWUNDO) 建立的「<磁碟>:\$Recycle.Bin\<群組>\{original-path.txt, item}」結構。
namespace x64arm {
std::span<const WindowsServiceDescriptor> shell_path_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"shell32.dll","DriveType",1,0,0,0x1},{"shell32.dll","RealDriveType",2,0,0,0x3},{"shell32.dll","IsNetDrive",1,0,0,0x1},
        {"shell32.dll","IsLFNDriveA",1},{"shell32.dll","IsLFNDriveW",1},{"shell32.dll","PathCleanupSpec",2},{"shell32.dll","PathGetShortPath",1},
        {"shell32.dll","PathIsExe",1},{"shell32.dll","PathMakeUniqueName",5,0,0,0x2},{"shell32.dll","PathYetAnotherMakeUniqueName",4},
        {"shell32.dll","PathQualify",1},{"shell32.dll","PathResolve",3,0,0,0x4},
        {"shell32.dll","SHPathPrepareForWriteA",4,0,0,0x8},{"shell32.dll","SHPathPrepareForWriteW",4,0,0,0x8},{"shell32.dll","SHValidateUNC",3,0,0,0x4},
        {"shell32.dll","CheckEscapesA",2,0,0,0x2},{"shell32.dll","CheckEscapesW",2,0,0,0x2},
        {"shell32.dll","SheChangeDirA",1},{"shell32.dll","SheChangeDirW",1},{"shell32.dll","SheGetDirA",2,0,0,0x1},{"shell32.dll","SheGetDirW",2,0,0,0x1},
        {"shell32.dll","SHQueryRecycleBinA",2},{"shell32.dll","SHQueryRecycleBinW",2},{"shell32.dll","SHEmptyRecycleBinA",3,0,0,0x4},{"shell32.dll","SHEmptyRecycleBinW",3,0,0,0x4},
        {"shell32.dll","SHUpdateRecycleBinIcon",0},
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_shell_paths(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const bool narrow=name.ends_with('A');
    const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto u32=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    const auto host=[&](std::string_view symbol,std::initializer_list<std::uint64_t> args){return nested_service(state,memory,"kernel32.dll",symbol,args)&UINT32_MAX;};
    const auto read=[&](std::uint64_t at)->std::u16string{if(!at)return {};return narrow?utf8_to_utf16_lossy(read_ansi(memory,at,32768)):read_utf16(memory,GuestAddress{at},32768);};
    // 寫回 NUL 結尾字串（A：UTF-8），capacity 以單位計且含 NUL；放不下時回 false。
    const auto write=[&](std::uint64_t at,const std::u16string& text,std::size_t capacity=260)->bool{
        if(narrow){const auto bytes=utf16_to_utf8_lossy(text);if(bytes.size()>=capacity)return false;std::vector<std::uint8_t> data(bytes.begin(),bytes.end());data.push_back(0);memory.write(GuestAddress{at},data);return true;}
        if(text.size()>=capacity)return false;memory.write(GuestAddress{at},utf16_bytes(text));return true;};
    GuestScratch temporary(*this,memory,state);
    const auto attributes=[&](const std::u16string& path)->std::uint32_t{const auto text=temporary.text(path);return text?static_cast<std::uint32_t>(host("GetFileAttributesW",{text})):UINT32_MAX;};
    const auto exists=[&](const std::u16string& path){return !path.empty() && attributes(path)!=UINT32_MAX;};
    const auto full_path=[&](const std::u16string& path)->std::optional<std::u16string>{
        const auto text=temporary.text(path),buffer=temporary.allocate(2*32768);if(!text || !buffer)return std::nullopt;
        const auto length=host("GetFullPathNameW",{text,32767,buffer,0});if(!length || length>=32767)return std::nullopt;return read_utf16(memory,GuestAddress{buffer},32767);};
    const auto drive_type=[&](std::uint32_t drive)->std::uint32_t{if(drive>25)return 1;const auto root=temporary.text(std::u16string{static_cast<char16_t>(u'A'+drive),u':',u'\\'});return root?static_cast<std::uint32_t>(host("GetDriveTypeW",{root})):1U;};
    // 路徑的根（"C:\"、"\\server\share\"）；相對路徑回空字串（代表目前磁碟）。
    const auto root_of=[](const std::u16string& path)->std::u16string{
        if(path.size()>=2 && path[1]==u':')return path.substr(0,2)+u"\\";
        if(path.starts_with(u"\\\\")){const auto server=path.find(u'\\',2);if(server==path.npos)return {};const auto share=path.find(u'\\',server+1);return (share==path.npos?path:path.substr(0,share))+u"\\";}
        return {};};
    const auto long_names=[&](const std::u16string& path)->bool{
        const auto root=root_of(path);const auto text=root.empty()?0:temporary.text(root),maximum=temporary.allocate(4);if(!maximum || (!root.empty() && !text))return false;
        return host("GetVolumeInformationW",{text,0,0,0,maximum,0,0,0}) && memory.load(GuestAddress{maximum},32)>12;};
    const auto extension_of=[](const std::u16string& path)->std::u16string{const auto slash=path.find_last_of(u"\\/:");const auto dot=path.rfind(u'.');
        if(dot==path.npos || (slash!=path.npos && dot<slash))return {};auto out=path.substr(dot);for(auto& c:out)c=unicode_case_unit(c,false);return out;};
    const auto join=[](const std::u16string& directory,const std::u16string& file){if(directory.empty())return file;return directory.back()==u'\\'?directory+file:directory+u"\\"+file;};
    // 在目錄中產生不存在的名稱：先試原名，再於副檔名前加 " (n)"（n 從 2 起）。
    const auto unique_name=[&](const std::u16string& directory,const std::u16string& base)->std::optional<std::u16string>{
        if(base.empty())return std::nullopt;const auto candidate=join(directory,base);if(!exists(candidate))return candidate;
        const auto dot=base.rfind(u'.');const auto stem=dot==base.npos || !dot?base:base.substr(0,dot),extension=dot==base.npos || !dot?std::u16string{}:base.substr(dot);
        for(unsigned n=2;n<1000;++n){const auto digits=std::to_string(n);auto next=join(directory,stem+u" ("+std::u16string(digits.begin(),digits.end())+u")"+extension);if(!exists(next))return next;}
        return std::nullopt;};
    if(name=="DriveType" || name=="RealDriveType")return done(drive_type(u32(0)));
    if(name=="IsNetDrive")return done(drive_type(u32(0))==4?1U:0U); // DRIVE_REMOTE：已連線的網路磁碟
    if(name.starts_with("IsLFNDrive"))return done(long_names(read(arg(0)))?1U:0U);
    if(name=="PathIsExe"){
        // 可直接執行的副檔名。
        static constexpr std::u16string_view executable[]{u".exe",u".com",u".pif",u".cmd",u".bat",u".scf",u".scr"};
        const auto extension=extension_of(read(arg(0)));return done(std::find(std::begin(executable),std::end(executable),extension)!=std::end(executable)?1U:0U);
    }
    if(name=="PathGetShortPath" || name=="PathQualify"){
        // 原地換成短檔名／完整路徑；失敗時保持原狀。
        if(!arg(0))return done(0);const auto path=read(arg(0));std::optional<std::u16string> result;
        if(name=="PathQualify")result=full_path(path);
        else {const auto text=temporary.text(path),buffer=temporary.allocate(2*260);if(text && buffer){const auto length=host("GetShortPathNameW",{text,buffer,260});if(length && length<260)result=read_utf16(memory,GuestAddress{buffer},259);}}
        if(result)(void)write(arg(0),*result);return done(0);
    }
    if(name=="PathCleanupSpec"){
        // (目錄, 檔名規格 輸入輸出)：移除非法字元（PCS_REMOVEDCHAR 2）；目錄不支援長檔名時空白改底線（PCS_REPLACEDCHAR 1）並截成 8.3（PCS_TRUNCATED 4）；
        // 目錄加檔名超過 MAX_PATH 時截短檔名（PCS_TRUNCATED），完全放不下回 PCS_FATAL｜PCS_PATHTOOLONG。
        if(!arg(1))return done(0x80000000U);const auto directory=read(arg(0));auto spec=read(arg(1));std::uint32_t flags=0;
        std::u16string clean;for(const auto c:spec){if(c<32 || std::u16string_view(u"\\/:*?\"<>|").find(c)!=std::u16string_view::npos){flags|=2;continue;}clean.push_back(c);}
        if(!directory.empty() && !long_names(directory)){
            for(auto& c:clean)if(c==u' '){c=u'_';flags|=1;}
            const auto dot=clean.rfind(u'.');auto stem=dot==clean.npos?clean:clean.substr(0,dot),extension=dot==clean.npos?std::u16string{}:clean.substr(dot+1);
            if(stem.size()>8){stem.resize(8);flags|=4;}if(extension.size()>3){extension.resize(3);flags|=4;}clean=extension.empty()?stem:stem+u"."+extension;
        }
        const auto prefix=directory.empty()?0:directory.size()+(directory.back()==u'\\'?0:1);
        if(prefix+clean.size()>=260){if(prefix>=259)return done(0x80000008U);clean.resize(259-prefix);flags|=4;}
        (void)write(arg(1),clean);return done(flags);
    }
    if(name=="PathMakeUniqueName" || name=="PathYetAnotherMakeUniqueName"){
        // PathMakeUniqueName(out, cch, 短樣板, 長樣板, 目錄)：支援長檔名時用長樣板；PathYetAnotherMakeUniqueName(out MAX_PATH, 目錄, 短名, 長名)。
        const bool yet=name=="PathYetAnotherMakeUniqueName";if(!arg(0))return done(0);
        const auto directory=read(arg(yet?1:4)),short_name=read(arg(yet?2:2)),long_name=read(arg(3));const auto capacity=yet?260U:u32(1);
        const auto base=!long_name.empty() && (directory.empty() || long_names(directory))?long_name:short_name;
        const auto result=unique_name(directory,base);if(!result || !write(arg(0),*result,capacity))return done(0);return done(1);
    }
    if(name=="PathResolve"){
        // (路徑 輸入輸出 MAX_PATH, NULL 結尾目錄陣列, PRF_*)：VERIFYEXISTS 1、TRYPROGRAMEXTENSIONS 3、FIRSTDIRDEF 4、DONTFINDLNK 8、REQUIREABSOLUTE 16。
        if(!arg(0))return done(0);const auto path=read(arg(0));const auto flags=u32(2);const bool verify=(flags&1U)!=0,extensions=(flags&3U)==3U;
        std::vector<std::u16string> directories;
        if(arg(1)){const unsigned unit=x86_?4U:8U;for(unsigned i=0;i<64;++i){const auto entry=memory.load(GuestAddress{arg(1)+i*unit},static_cast<std::uint8_t>(unit*8));if(!entry)break;directories.push_back(read_utf16(memory,GuestAddress{entry},32768));}}
        const auto found=[&](const std::u16string& candidate)->std::optional<std::u16string>{
            if(exists(candidate))return candidate;if(!extensions || !extension_of(candidate).empty())return std::nullopt;
            for(const auto extension:{u".pif",u".com",u".exe",u".bat",u".lnk",u".cmd"}){if((flags&8U) && std::u16string_view(extension)==u".lnk")continue;if(exists(candidate+extension))return candidate+extension;}
            return std::nullopt;};
        const bool bare=path.find_first_of(u"\\/:")==path.npos;
        if(bare){
            if(flags&16U){last_error_=2;return done(0);}
            for(const auto& directory:directories)if(auto hit=found(join(directory,path))){if(auto full=full_path(*hit);full && write(arg(0),*full))return done(1);}
            {const auto text=temporary.text(path),buffer=temporary.allocate(2*260);
                if(text && buffer){const auto length=host("SearchPathW",{0,text,0,260,buffer,0});if(length && length<260){const auto hit=read_utf16(memory,GuestAddress{buffer},259);if(write(arg(0),hit))return done(1);}}
                if(extensions)for(const auto extension:{u".pif",u".com",u".exe",u".bat",u".cmd"}){const auto name_text=temporary.text(path+extension);if(!name_text || !buffer)break;
                    const auto length=host("SearchPathW",{0,name_text,0,260,buffer,0});if(length && length<260 && write(arg(0),read_utf16(memory,GuestAddress{buffer},259)))return done(1);}}
            if(verify){last_error_=2;return done(0);}
            const auto base=(flags&4U) && !directories.empty()?join(directories.front(),path):path;if(auto full=full_path(base);full && write(arg(0),*full))return done(1);return done(0);
        }
        auto full=full_path(path);if(!full)return done(0);
        if(verify){const auto hit=found(*full);if(!hit){last_error_=2;return done(0);}full=hit;}
        return done(write(arg(0),*full)?1U:0U);
    }
    if(name.starts_with("SHPathPrepareForWrite")){
        // (HWND, punkEnableModless, 路徑, SHPPFW_*)：DIRCREATE 1／ASKDIRCREATE 2 時建立缺少的目錄（不顯示詢問），IGNOREFILENAME 4 先去掉最後一段，MEDIACHECKONLY 0x10 只檢查根目錄。
        auto path=read(arg(2));const auto flags=u32(3);if(path.empty())return done(0x80070057U);
        if(flags&4U){const auto slash=path.find_last_of(u'\\');if(slash!=path.npos)path.resize(std::max<std::size_t>(slash,root_of(path).size()));}
        if(flags&0x10U){const auto root=root_of(path);return done(root.empty() || exists(root)?0U:0x80070015U);} // ERROR_NOT_READY
        const auto current=attributes(path);if(current!=UINT32_MAX)return done((current&0x10U)?0U:0x8007010bU); // ERROR_DIRECTORY
        if(!(flags&3U))return done(0x80070003U);
        const auto text=temporary.text(path);if(!text)return done(0x8007000eU);
        const auto error=static_cast<std::uint32_t>(nested_service(state,memory,"shell32.dll","SHCreateDirectoryExW",{arg(0),text,0}));
        return done(!error || error==183?0U:0x80070000U|(error&0xffffU));
    }
    if(name=="SHValidateUNC"){
        // (HWND, UNC 路徑, VALIDATEUNC_*)：只接受 \\server\share 形式，且共用根可以存取。
        const auto path=read_utf16(memory,GuestAddress{arg(1)},32768);const auto root=path.starts_with(u"\\\\")?root_of(path):std::u16string{};
        if(root.size()<5 || root.find(u'\\',2)==root.size()-1){last_error_=67;return done(0);}if(!exists(root)){last_error_=67;return done(0);}return done(1); // ERROR_BAD_NET_NAME
    }
    if(name.starts_with("CheckEscapes")){
        // (字串 輸入輸出, 容量)：含空白或命令列特殊字元時加上雙引號；回傳處理後的長度（放不下時不變）。
        if(!arg(0))return done(0);const auto text=read(arg(0));const auto capacity=u32(1);
        if(text.size()>=2 && text.front()==u'"' && text.back()==u'"')return done(text.size());
        if(text.find_first_of(u" \t,;^=&()")==text.npos)return done(text.size());const auto quoted=u"\""+text+u"\"";
        if(!write(arg(0),quoted,capacity))return done(text.size());return done(quoted.size());
    }
    if(name.starts_with("SheChangeDir")){const auto text=temporary.text(read(arg(0)));if(!text)return done(8);return done(host("SetCurrentDirectoryW",{text})?0U:last_error_);}
    if(name.starts_with("SheGetDir")){
        // (磁碟 1＝A…，0＝目前磁碟, 緩衝區 MAX_PATH)：回傳 0 表示成功。
        const auto drive=u32(0);if(!arg(1) || drive>26)return done(87);std::optional<std::u16string> path;
        if(!drive){const auto buffer=temporary.allocate(2*260);if(buffer){const auto length=host("GetCurrentDirectoryW",{260,buffer});if(length && length<260)path=read_utf16(memory,GuestAddress{buffer},259);}}
        else path=full_path(std::u16string{static_cast<char16_t>(u'A'+drive-1),u':',u'.'});
        if(!path)return done(last_error_?last_error_:15);return done(write(arg(1),*path)?0U:111U);
    }
    if(name.starts_with("SHQueryRecycleBin") || name.starts_with("SHEmptyRecycleBin") || name=="SHUpdateRecycleBinIcon"){
        if(name=="SHUpdateRecycleBinIcon"){(void)nested_service(state,memory,"shell32.dll","SHChangeNotify",{0x8000,0x0003,31,0});return done(0);} // SHCNE_UPDATEIMAGE、SHCNF_DWORD
        const bool query=name.starts_with("SHQueryRecycleBin");const auto root=read(arg(query?0:1));
        std::vector<std::string> bins;
        if(root.empty()){const auto mask=host("GetLogicalDrives",{});for(unsigned i=0;i<26;++i)if(mask&(1U<<i))bins.push_back(std::string(1,static_cast<char>('A'+i))+":/$Recycle.Bin");}
        else if(root.size()>=2 && root[1]==u':')bins.push_back(std::string(1,static_cast<char>(unicode_case_unit(root[0],true)))+":/$Recycle.Bin");
        else return done(0x80070057U);
        // 每個群組目錄是一個項目，大小為其中 item 的檔案或目錄總大小。
        const std::function<std::uint64_t(const std::string&)> size_of=[&](const std::string& path)->std::uint64_t{
            const auto info=filesystem_.information(path);if(info.error)return 0;if(!(info.metadata.attributes&0x10U))return info.metadata.size;
            std::uint64_t total=0;for(const auto& entry:filesystem_.list(path).entries)total+=size_of(path+"/"+entry.name);return total;};
        std::uint64_t items=0,bytes=0;std::vector<std::string> groups;
        for(const auto& bin:bins){const auto listing=filesystem_.list(bin);if(listing.error)continue;
            for(const auto& entry:listing.entries){if(!(entry.metadata.attributes&0x10U))continue;groups.push_back(bin+"/"+entry.name);++items;bytes+=size_of(bin+"/"+entry.name+"/item");}}
        if(query){
            // SHQUERYRBINFO：cbSize、i64Size、i64NumItems（x86 為 1 位元組對齊的 20 位元組，x64 為 24 位元組）。
            if(!arg(1))return done(0x80070057U);const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32));if(size!=(x86_?20U:24U))return done(0x80070057U);
            memory.store(GuestAddress{arg(1)+(x86_?4U:8U)},64,bytes);memory.store(GuestAddress{arg(1)+(x86_?12U:16U)},64,items);return done(0);
        }
        if(!items)return done(0);
        // 只捕捉 this：確認對話方塊之後在協程中執行時，外層函式的區域變數已不存在。
        const auto remove_all=[this](const std::vector<std::string>& targets)->std::uint32_t{
            std::function<std::uint32_t(const std::string&)> erase;erase=[&](const std::string& path)->std::uint32_t{
                const auto info=filesystem_.information(path);if(info.error)return info.error==2?0:info.error;
                if(info.metadata.attributes&0x10U){for(const auto& entry:filesystem_.list(path).entries)if(const auto error=erase(path+"/"+entry.name))return error;return filesystem_.remove(path,true);}
                return filesystem_.remove(path,false);};
            for(const auto& group:targets)if(const auto error=erase(group))return 0x80070000U|(error&0xffffU);return 0;};
        if(u32(2)&1U)return done(remove_all(groups)); // SHERB_NOCONFIRMATION
        // 未指定 SHERB_NOCONFIRMATION 時先確認；取消回 ERROR_CANCELLED。
        const auto count=std::to_string(items);
        return nested_continuation(state,memory,"SHEmptyRecycleBin",[this,service,count,groups,remove_all](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);GuestScratch scratch(*this,m,s);
            const auto text=scratch.text(u"確定要永久刪除回收區中的 "+std::u16string(count.begin(),count.end())+u" 個項目嗎？"),title=scratch.text(u"清理回收區");
            const auto stub=host_service_stub("user32.dll","MessageBoxW");const auto target=stub.value?stub.value:resolve("user32.dll","MessageBoxW").value;
            const auto answer=text && title?co_await service_callback::call(target,{call(0),text,title,0x34}):7U; // MB_YESNO｜MB_ICONWARNING
            call.return_to(s,answer==6?std::uint64_t{remove_all(groups)}:std::uint64_t{0x800704c7U});co_return std::nullopt;
        });
    }
    throw HostCallFault("未知的 shell32 路徑服務");
}
}
