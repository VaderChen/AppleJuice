if(service.family=="internal-shell-path-search" || name.starts_with("PathFindOnPath")){
    return nested_continuation(state,memory,"ShellPathSearch",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall arg(s,m,service);
        const auto name=service.symbol;
        const bool wide=name.ends_with('W');
        const unsigned pointer_size=x86_?4U:8U;
        const auto bits=static_cast<std::uint8_t>(pointer_size*8);
        const auto done=[&](bool result){arg.return_to(s,result);return std::optional<std::uint32_t>{};};
        const auto read_path=[&](std::uint64_t at){return wide?read_utf16(m,GuestAddress{at},32768):utf8_to_utf16_lossy(read_ansi(m,at,32768));};
        const bool search=name.starts_with("PathFindOnPath");
        const bool defext=name.starts_with("PathFileExistsDefExt");
        const bool with_attributes=name.find("AndAttributes")!=name.npos;
        const auto output=with_attributes?arg(defext?2:1):0;
        if(output && (!wide || defext))m.store(GuestAddress{output},32,UINT32_MAX);
        if(!arg(0))co_return done(0);
        const auto input=read_path(arg(0));
        const auto flags=defext?static_cast<std::uint32_t>(arg(1)):name.starts_with("PathFindOnPathEx")?static_cast<std::uint32_t>(arg(2)):0U;
        if(search && input.find_first_of(u"\\:")!=input.npos)co_return done(0);
        const auto get_attributes=host_service_stub("kernel32.dll","GetFileAttributesW").value;
        const auto attributes=[&](const std::u16string& path)->service_callback::Task<std::uint32_t>{
            GuestScratch temporary(*this,m,s);
            const auto text=temporary.text(path);
            if(!text)co_return UINT32_MAX;
            co_return co_await service_callback::call(get_attributes,{text});
        };
        const auto match=[&](std::u16string path)->service_callback::Task<bool>{
            if(path.empty())co_return false;
            const bool has_extension=windows_extension_offset(path)<path.size();
            const bool plain_attributes=with_attributes && (!flags || ((flags&0x40U) && has_extension));
            if(!plain_attributes && path.size()>=260)co_return false;
            if(!plain_attributes && path.starts_with(u"\\\\") && !path.starts_with(u"\\\\?\\")){
                const auto share=path.find(u'\\',2);
                if(share==path.npos || path.find(u'\\',share+1)==path.npos)co_return false;
            }
            const bool append=flags && (!has_extension || ((flags&0x40U) && !with_attributes));
            auto attr=UINT32_MAX;
            if(append){
                if(path.size()>255)co_return false;
                constexpr std::u16string_view exts[]{u".pif",u".com",u".exe",u".bat",u".lnk",u".cmd"};
                const auto prefix=path.size();
                for(unsigned i=0;i<std::size(exts);++i){
                    if(!(flags&(1U<<i)))continue;
                    path.resize(prefix);path+=exts[i];
                    attr=co_await attributes(path);
                    if(attr!=UINT32_MAX)break;
                }
            }else attr=co_await attributes(path);
            if(attr==UINT32_MAX)co_return false;
            if(search || append){
                if(wide)m.write(GuestAddress{arg(0)},utf16_bytes(path));
                else{
                    const auto text=utf16_to_utf8_lossy(path);
                    if(text.size()>=260)co_return false;
                    std::vector<std::uint8_t> bytes(text.begin(),text.end());bytes.push_back(0);
                    m.write(GuestAddress{arg(0)},bytes);
                }
            }
            if(output)m.store(GuestAddress{output},32,attr);
            co_return true;
        };
        if(!search){
            const bool found=co_await match(input);
            if(!found && output)m.store(GuestAddress{output},32,UINT32_MAX);
            co_return done(found);
        }
        const auto in_directory=[&](const std::u16string& dir)->service_callback::Task<bool>{
            std::u16string path;
            if(canonicalize(combine_path(dir,input),0,260,path))co_return false;
            co_return co_await match(std::move(path));
        };
        if(arg(1)){
            for(auto slot=arg(1);;slot+=pointer_size){
                const auto pointer=m.load(GuestAddress{slot},bits);
                if(!pointer)break;
                const auto dir=read_path(pointer);
                if(dir.empty())break;
                if(co_await in_directory(dir))co_return done(1);
            }
        }
        if(co_await in_directory(windows_system_directory) || co_await in_directory(combine_path(windows_directory,u"System")) ||
           co_await in_directory(windows_directory))co_return done(1);
        const auto env=runtime_.environment.find(u"PATH");
        if(env!=runtime_.environment.end() && !env->second.second.empty()){
            const auto paths=env->second.second;
            for(std::size_t start=0;;){
                const auto end=paths.find(u';',start);
                while(start<paths.size() && paths[start]==u' ')++start;
                if(co_await in_directory(paths.substr(start,end==paths.npos?paths.npos:end-start)))co_return done(1);
                if(end==paths.npos)break;
                start=end+1;
            }
        }
        co_return done(0);
    });
}
