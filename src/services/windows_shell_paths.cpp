#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_paths.hpp"
#include "x64arm/windows_system_paths.hpp"
#include "windows_path_character.h"
#include "service_callback_task.hpp"
#include "shell_extension_text.h"
#include "mpr_state.hpp"
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
        {"shell32.dll","#65",1,0,0,0x1},{"shell32.dll","RealDriveTypeFlags",2,0,0,0x3},
        {"shell32.dll","IsLFNDriveA",1},{"shell32.dll","IsLFNDriveW",1},{"shell32.dll","PathCleanupSpec",2},{"shell32.dll","PathGetShortPath",1},
        {"shell32.dll","PathIsExe",1},{"shell32.dll","PathMakeUniqueName",5,0,0,0x2},{"shell32.dll","PathYetAnotherMakeUniqueName",4},
        {"shell32.dll","PathQualify",1},{"shell32.dll","PathResolve",3,0,0,0x4},
        {"shell32.dll","SHPathPrepareForWriteA",4,0,0,0x8},{"shell32.dll","SHPathPrepareForWriteW",4,0,0,0x8},
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
    const auto full_path=[&](const std::u16string& path)->std::optional<std::u16string>{
        const auto text=temporary.text(path),buffer=temporary.allocate(2*32768);if(!text || !buffer)return std::nullopt;
        const auto length=host("GetFullPathNameW",{text,32767,buffer,0});if(!length || length>=32767)return std::nullopt;return read_utf16(memory,GuestAddress{buffer},32767);};
    const auto drive_type=[&](std::uint32_t drive)->std::uint32_t{if(drive>25)return 1;const auto root=temporary.text(std::u16string{static_cast<char16_t>(u'A'+drive),u':',u'\\'});return root?static_cast<std::uint32_t>(host("GetDriveTypeW",{root})):1U;};
    // 路徑的根（"C:\"、"\\server\share\"）；相對路徑回空字串（代表目前磁碟）。
    const auto root_of=[](const std::u16string& path)->std::u16string{
        if(path.size()>=2 && path[1]==u':')return path.substr(0,2)+u"\\";
        if(path.starts_with(u"\\\\")){const auto server=path.find(u'\\',2);if(server==path.npos)return {};const auto share=path.find(u'\\',server+1);return (share==path.npos?path:path.substr(0,share))+u"\\";}
        return {};};
    const auto volume_long_names=[&](std::uint64_t root,bool ansi)->bool{
        const auto maximum=temporary.allocate(4);
        if(!maximum){last_error_=8;return false;}
        return host(ansi?"GetVolumeInformationA":"GetVolumeInformationW",{root,0,0,0,maximum,0,0,0}) &&
               memory.load(GuestAddress{maximum},32)>12;
    };
    const auto long_names=[&](const std::u16string& path)->bool{
        const auto root=root_of(path);
        const auto text=root.empty()?0:temporary.text(root);
        return (root.empty() || text) && volume_long_names(text,false);
    };
    const auto extension_of=[](const std::u16string& path)->std::u16string{const auto slash=path.find_last_of(u"\\/:");const auto dot=path.rfind(u'.');
        if(dot==path.npos || (slash!=path.npos && dot<slash))return {};auto out=path.substr(dot);for(auto& c:out)c=unicode_case_unit(c,false);return out;};
    const auto join=[](const std::u16string& directory,const std::u16string& file){if(directory.empty())return file;return directory.back()==u'\\'?directory+file:directory+u"\\"+file;};
    if(name=="#65"){
        refresh_file_security_context();
        if(mpr_){
            auto scope=mpr_->drive_capabilities.find(filesystem_.network_scope());
            if(scope!=mpr_->drive_capabilities.end()){
                if(static_cast<std::int32_t>(u32(0))<0)mpr_->drive_capabilities.erase(scope);
                else scope->second.erase(u32(0));
            }
        }
        return done(0);
    }
    if(name=="DriveType" || name=="RealDriveType" || name=="RealDriveTypeFlags" || name=="IsNetDrive"){
        unsigned drive=u32(0);
        const bool letter_query=(name=="RealDriveType" || name=="DriveType") && drive>='A' && drive<='Z';
        if(letter_query)drive-='A';
        refresh_file_security_context();
        const auto type=drive_type(drive);
        if(name=="IsNetDrive")return done(type==4);
        if(!letter_query && name!="RealDriveTypeFlags")return done(type);
        if(drive>=26)return done(0);

        constexpr unsigned long_name_flag=0x20, shell_open=0x400, security=0x800;
        constexpr unsigned compressed=0x1000, compressible=0x2000;
        unsigned flags=type | (type==5?0:shell_open);
        const bool probe_volume=name=="DriveType" || u32(1)!=0;
        if(!mpr_)mpr_=std::make_shared<MprState>();
        auto& cache=mpr_->drive_capabilities[filesystem_.network_scope()];
        auto cached=cache.find(drive);
        const bool reuse=cached!=cache.end() && cached->second.type==type;
        if(reuse){
            flags=cached->second.flags;
            if(!probe_volume || cached->second.probed)return done(flags);
        }
        const std::u16string path{static_cast<char16_t>(u'A'+drive),u':',u'\\'};
        const auto root=temporary.text(path);
        if(!root)return done(flags);
        if(!reuse && type==1){
            const auto letter=temporary.text(path.substr(0,2)),buffer=temporary.allocate(4),size=temporary.allocate(4);
            if(letter && buffer && size){
                memory.store(GuestAddress{size},32,2);
                if(nested_service(state,memory,"mpr.dll","WNetGetConnectionW",{letter,buffer,size})==1201)flags|=0x200;
            }
        }
        if(!reuse && type==3){
            const auto inf=temporary.text(path+u"autorun.inf");
            if(inf && host("GetFileAttributesW",{inf})!=UINT32_MAX){
                const auto restricted=[&](unsigned policy){
                    return nested_service(state,memory,"shell32.dll","SHRestricted",{policy});
                };
                if(!(restricted(0x200)&(1U<<drive)) && !(restricted(0x400)&(1U<<type))){
                    const auto keys=temporary.allocate(1024);
                    const auto has_keys=[&](const char16_t* section){
                        const auto section_name=temporary.text(section);
                        return section_name && keys && host("GetPrivateProfileStringW",{section_name,0,0,keys,512,inf})>=4;
                    };
                    if(has_keys(x86_?u"autorun.x86":u"autorun.amd64") || has_keys(u"autorun"))flags|=0x140;
                }
            }
        }
        auto& capabilities=cache.insert_or_assign(drive,MprState::DriveCapabilities{type,flags,false}).first->second;
        if(probe_volume && type>2){
            const auto info=temporary.allocate(8);
            if(root && info && host("GetVolumeInformationW",{root,0,0,0,info,info+4,0,0})){
                const auto volume_flags=memory.load(GuestAddress{info+4},32);
                if(memory.load(GuestAddress{info},32)>12)flags|=long_name_flag;
                if(volume_flags&8)flags|=security;
                if(volume_flags&0x10){
                    flags|=compressible;
                    const auto attrs=host("GetFileAttributesW",{root});
                    if(attrs!=UINT32_MAX && (attrs&0x800))flags|=compressed;
                }
                capabilities={type,flags,true};
            }
        }
        return done(flags);
    }
    if(name.starts_with("IsLFNDrive"))return done(volume_long_names(arg(0),narrow)?1U:0U);
    if(name=="PathIsExe"){
        return done(shell_is_exe(&memory,arg(0),[](void* ctx,unsigned long long at,unsigned unit){
            return static_cast<unsigned>(static_cast<GuestMemory*>(ctx)->load(GuestAddress{at},static_cast<std::uint8_t>(unit*8)));
        }));
    }
    if(name=="PathGetShortPath" || name=="PathQualify"){
        // 原地換成短檔名／完整路徑；失敗時保持原狀。
        if(!arg(0))return done(0);const auto path=read(arg(0));std::optional<std::u16string> result;
        if(name=="PathQualify")result=full_path(path);
        else {const auto text=temporary.text(path),buffer=temporary.allocate(2*260);if(text && buffer){const auto length=host("GetShortPathNameW",{text,buffer,260});if(length && length<260)result=read_utf16(memory,GuestAddress{buffer},259);}}
        if(result)(void)write(arg(0),*result);return done(0);
    }
    if(name=="PathCleanupSpec"){
        constexpr unsigned replaced=1,removed=2,truncated=4,too_long=0x80000008U;
        if(!arg(1))return done(too_long);
        const auto directory=read(arg(0));
        const bool short_names=!long_names(arg(0)?directory:windows_directory);
        std::u16string clean;
        unsigned flags=0;
        for(const auto c:read(arg(1))){
            const auto type=path_character_type(c);
            if(type==0 || type==4 || type==8 || (short_names && (c==u';' || c==u','))){
                flags|=removed;
            }else if(short_names && c==u' '){
                clean.push_back(u'_');
                flags|=replaced;
            }else clean.push_back(c);
        }
        if(short_names){
            const auto dot=clean.rfind(u'.');
            auto stem=clean.substr(0,dot);
            auto ext=dot==clean.npos?std::u16string{}:clean.substr(dot+1);
            if(std::erase(stem,u'.') || (dot!=clean.npos && ext.empty()))flags|=removed;
            if(stem.size()>8){stem.resize(8);flags|=truncated;}
            if(ext.size()>3){ext.resize(3);flags|=truncated;}
            clean=ext.empty()?stem:stem+u"."+ext;
        }
        if(directory.size()+clean.size()>=260)return done(flags|too_long);
        write(arg(1),clean);
        return done(flags);
    }
    if(name=="PathMakeUniqueName" || name=="PathYetAnotherMakeUniqueName"){
        // PathMakeUniqueName(out, cch, 短樣板, 長樣板, 目錄)：支援長檔名時用長樣板；PathYetAnotherMakeUniqueName(out MAX_PATH, 目錄, 短名, 長名)。
        const bool yet=name=="PathYetAnotherMakeUniqueName";
        const unsigned capacity=yet?260U:u32(1);
        const auto directory=arg(yet?1:4);
        const auto short_name=arg(2),long_name=arg(3);
        const auto file=long_name && (yet || !directory || long_names(read(directory)))?long_name:short_name;
        if(!arg(0) || !capacity || !file || read(file).empty())return done(0);
        const auto buffer=temporary.allocate(260*sizeof(char16_t));
        if(!buffer || !nested_service(state,memory,"shlwapi.dll","PathCombineW",{buffer,directory,file}))return done(0);
        const auto original=read(buffer);
        const auto extension_at=nested_service(state,memory,"shlwapi.dll","PathFindExtensionW",{buffer});
        if(extension_at<buffer || extension_at>buffer+original.size()*2)return done(0);
        const auto split=(extension_at-buffer)/2;
        const auto stem=original.substr(0,split),extension=original.substr(split);
        auto candidate=original;
        for(std::uint64_t suffix=2;candidate.size()<capacity;++suffix){
            if(host("GetFileAttributesW",{buffer})==UINT32_MAX)return done(write(arg(0),candidate,capacity));
            const auto digits=std::to_string(suffix);
            candidate=stem+u" ("+std::u16string(digits.begin(),digits.end())+u")"+extension;
            if(!write(buffer,candidate,260))break;
        }
        return done(0);
    }
    if(name=="PathResolve"){
        // (路徑 輸入輸出 MAX_PATH, NULL 結尾目錄陣列, PRF_*)：VERIFYEXISTS 1、TRYPROGRAMEXTENSIONS 3、FIRSTDIRDEF 4、DONTFINDLNK 8、REQUIREABSOLUTE 16。
        if(!arg(0))return done(0);
        const auto path=read(arg(0));
        const auto flags=u32(2);
        const auto type=windows_dos_path_type(path);
        const auto extension=extension_of(path);
        const auto missing=[&]{last_error_=2;return done(0);};
        if(path.empty() || ((flags&8U) && extension==u".lnk"))return missing();
        if((flags&16U) && type!=WindowsDosPathType::DriveAbsolute && type!=WindowsDosPathType::UncAbsolute &&
           type!=WindowsDosPathType::LocalDevice)return missing();
        const bool bare=path.find_first_of(u"\\/:")==path.npos;
        const bool try_extensions=(flags&3U)==3U && extension.empty();
        const auto probe=[&](const std::u16string& candidate,bool search)->std::optional<std::u16string>{
            GuestScratch scratch(*this,memory,state);
            const auto output=search?scratch.allocate(260*2):0;
            if(search && !output)return std::nullopt;
            const auto lookup=[&](const std::u16string& file)->std::optional<std::u16string>{
                const auto input=scratch.text(file);
                if(!input)return std::nullopt;
                if(!search){
                    if(host("GetFileAttributesW",{input})!=UINT32_MAX)return file;
                }else{
                    const auto size=host("SearchPathW",{0,input,0,260,output,0});
                    if(size && size<260)return read_utf16(memory,GuestAddress{output},259);
                }
                return std::nullopt;
            };
            if(auto hit=lookup(candidate))return hit;
            if(try_extensions)for(const auto suffix:{u".pif",u".com",u".exe",u".bat",u".lnk",u".cmd"}){
                if((flags&8U) && std::u16string_view(suffix)==u".lnk")continue;
                if(auto hit=lookup(candidate+suffix))return hit;
            }
            return std::nullopt;
        };
        auto candidate=path;
        std::optional<std::u16string> hit;
        if(bare){
            const unsigned unit=x86_?4U:8U;
            for(auto slot=arg(1);slot;slot+=unit){
                const auto entry=memory.load(GuestAddress{slot},static_cast<std::uint8_t>(unit*8));
                if(!entry)break;
                const auto file=join(read(entry),path);
                if(slot==arg(1) && (flags&4U))candidate=file;
                hit=probe(file,false);
                if(hit)break;
            }
            if(!hit)hit=probe(path,true);
        }else if(flags&1U)hit=probe(path,false);
        if(!hit && (flags&1U))return missing();
        const auto full=full_path(hit?*hit:candidate);
        return done(full && write(arg(0),*full));
    }
    if(name.starts_with("SHPathPrepareForWrite")){
        return nested_continuation(state,memory,"SHPathPrepareForWrite",[this,service,root_of](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);
            const auto finish=[&](std::uint32_t hr){call.return_to(s,hr);return std::optional<std::uint32_t>{};};
            GuestScratch scratch(*this,m,s);
            if(!call(2))co_return finish(0x80070057U);
            auto path=service.symbol.ends_with('A')?utf8_to_utf16_lossy(read_ansi(m,call(2),32768)):read_utf16(m,GuestAddress{call(2)},32768);
            const auto flags=static_cast<std::uint32_t>(call(3));
            if(path.empty() || (flags&3U)==3U)co_return finish(0x80070057U);
            std::replace(path.begin(),path.end(),u'/',u'\\');
            if(flags&4U){
                const auto slash=path.find_last_of(u'\\');
                if(slash==path.npos)path=u".";
                else path.resize(std::max<std::size_t>(slash,root_of(path).size()));
            }
            const auto probe=[this,&path]()->std::uint32_t{
                const auto info=filesystem_.information(utf16_to_utf8(path));
                if(info.error)return info.error==2?3U:info.error;
                return (info.metadata.attributes&16U)?0U:267U;
            };
            auto error=probe();
            if(!error)co_return finish(0);
            const auto create=[this](GuestState& s,GuestMemory& m,std::uint64_t target)->std::uint32_t{
                const auto code=static_cast<std::uint32_t>(nested_service(s,m,"shell32.dll","SHCreateDirectoryExW",{0,target,0}));
                if(code!=183)return code;
                const auto info=filesystem_.information(utf16_to_utf8(read_utf16(m,GuestAddress{target})));
                return info.error?info.error:(info.metadata.attributes&16U)?0U:267U;
            };
            const bool ask=error==3 && (flags&2U);
            if(error==3 && (flags&1U)){
                const auto target=scratch.text(path);
                if(!target)co_return finish(0x8007000eU);
                error=create(s,m,target);
            }
            if(!error)co_return finish(0);
            const auto owner=call(0),unknown=call(1);
            if(!owner || (!ask && (flags&0x10U)))co_return finish(0x80070000U|(ask?1223U:error));

            const auto target=scratch.text(path),title=scratch.text(u"準備目錄");
            const auto question=scratch.text(u"要建立此目錄嗎？\n"+path),message=scratch.text(u"無法存取目錄：\n"+path);
            if(!target || !title || !question || !message){call.return_to(s,0x8007000eU);co_return std::nullopt;}
            const auto box=resolve("user32.dll","MessageBoxW").value;
            const auto toggle=resolve("shlwapi.dll","#355").value;
            if(unknown)(void)co_await service_callback::call(toggle,{unknown,0});
            auto code=error;
            if(ask){
                const auto answer=co_await service_callback::call(box,{owner,question,title,0x124U});
                code=answer==6?create(s,m,target):1223U;
            }
            if(code && code!=1223 && !(flags&0x10U))
                (void)co_await service_callback::call(box,{owner,message,title,0x10U});
            if(unknown)(void)co_await service_callback::call(toggle,{unknown,1});
            call.return_to(s,code?0x80070000U|code:0U);co_return std::nullopt;
        });
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
