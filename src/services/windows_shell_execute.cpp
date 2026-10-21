#include "x64arm/windows_services.hpp"
#include "x64arm/windows_paths.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>

namespace x64arm {
namespace {
struct ShellError {std::uint32_t code;};
std::u16string fold(std::u16string value){for(auto& c:value)c=unicode_case_unit(c,false);return value;}
std::uint32_t shell_error(std::uint32_t code){
    switch(code){case 2:case 3:case 5:case 8:return code;case 193:return 11;case 32:return 26;case 267:return 3;case 126:return 32;default:return 31;}
}
std::u16string quote_argument(std::u16string_view value){return quote_windows_argument(value);}
// 登錄 command 的位置參數保持引號語義；不經宿主 shell 解譯。
std::u16string substitute(std::u16string_view pattern,const std::u16string& target,const std::u16string& parameters){
    const auto arguments=parse_windows_arguments(parameters);std::u16string out;bool in_quotes=false;
    for(std::size_t i=0;i<pattern.size();++i){
        const auto c=pattern[i];if(c==u'"'){in_quotes=!in_quotes;out+=c;continue;}
        if(c!=u'%' || i+1==pattern.size()){out+=c;continue;}
        const auto next=pattern[i+1];
        if(next==u'%'){out+=u'%';++i;continue;}
        if(next==u'*'){out+=parameters;++i;continue;}
        std::u16string replacement;
        if(next==u'1' || next==u'l' || next==u'L')replacement=target;
        else if(next>=u'2' && next<=u'9'){const auto index=static_cast<std::size_t>(next-u'2');if(index<arguments.size())replacement=arguments[index];}
        else if(next==u'V' || next==u'v' || next==u'I' || next==u'i')throw ShellError{50};
        else{out+=c;continue;}
        // 已有引號時只放置其內文，避免把引號當成實際檔名。
        auto escaped=quote_argument(replacement);out+=in_quotes?escaped.substr(1,escaped.size()-2):escaped;++i;
        if(out.size()>32767)throw ShellError{206};
    }
    if(out.size()>32767)throw ShellError{206};return out;
}
}
std::optional<std::uint32_t> WindowsServices::invoke_shell_execute(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const bool wide=service.symbol.ends_with('W'),find=service.symbol.starts_with("FindExecutable"),extended=service.symbol.starts_with("ShellExecuteEx");
    const auto bits=static_cast<std::uint8_t>(x86_?32:64);const unsigned unit=x86_?4:8;
    const auto read=[&](std::uint64_t p){return !p?std::u16string{}:wide?read_utf16(memory,GuestAddress{p},32768):utf8_to_utf16(read_ansi(memory,p,32768));};
    std::uint64_t info=0,instance_at=0,process_at=0,arena=0,result=0;std::uint32_t flags=0,show=1;
    try {
        std::u16string verb,file,parameters,directory,class_name;std::uint64_t parent=0;
        if(extended){
            info=arg(0);if(!info || memory.load(GuestAddress{info},32)!=(x86_?60U:112U))throw ShellError{87};
            memory.check(GuestAddress{info},x86_?60:112,Access::Write);
            flags=static_cast<std::uint32_t>(memory.load(GuestAddress{info+4},32));
            const auto pointer=[&](unsigned index){return memory.load(GuestAddress{info+8+index*unit},bits);};
            parent=pointer(0);verb=read(pointer(1));file=read(pointer(2));parameters=read(pointer(3));directory=read(pointer(4));
            show=static_cast<std::uint32_t>(memory.load(GuestAddress{info+8+5*unit},32));
            instance_at=info+(x86_?32:56);process_at=info+(x86_?56:104);memory.store(GuestAddress{process_at},bits,0);
            // 靜態 verb、PIDL 與同步建立；DDE／COM extension／提權另有契約，不能假裝完成。
            constexpr std::uint32_t supported=1U|4U|0x40U|0x100U|0x400U|0x4000U|0x8000U|0x100000U|0x800000U;
            if(flags&~supported)throw ShellError{50};
            if(flags&1)class_name=read(memory.load(GuestAddress{info+(x86_?40:72)},bits));
            if((flags&1) && class_name.empty())throw ShellError{87};
            if(flags&4){const auto pidl=memory.load(GuestAddress{info+(x86_?36:64)},bits);const auto found=gui_.shell_pidls.find(pidl);if(found==gui_.shell_pidls.end() || !allocations_.contains(pidl))throw ShellError{87};file=found->second;}
        }else if(find){file=read(arg(0));directory=read(arg(1));if(!arg(2))throw ShellError{87};memory.store(GuestAddress{arg(2)},wide?16:8,0);}
        else{parent=arg(0);verb=read(arg(1));file=read(arg(2));parameters=read(arg(3));directory=read(arg(4));show=static_cast<std::uint32_t>(arg(5));}
        if(file.empty() || show>11)throw ShellError{87};if(parent && !gui_.windows.contains(parent))throw ShellError{1400};
        verb=fold(verb);const bool elevated=verb==u"runas";if(elevated)verb=u"open";
        directory=directory.empty()?runtime_.current_directory:normalized_windows_path(directory,runtime_.current_directory);
        const auto dir=filesystem_.information(utf16_to_utf8(directory));if(dir.error || !(dir.metadata.attributes&0x10))throw ShellError{267};
        const auto resolve_file=[&](std::u16string name,bool executable)->std::u16string{
            std::vector<std::u16string> names{name};if(executable && windows_extension_offset(name)==name.size())names.push_back(name+u".exe");
            std::vector<std::u16string> directories{directory};
            if(name.find_first_of(u"\\/:")==name.npos){directories.push_back(runtime_.application_directory);directories.push_back(u"C:\\Windows\\System32");directories.push_back(u"C:\\Windows");
                if(const auto path=runtime_.environment.find(u"PATH");path!=runtime_.environment.end()){const auto& list=path->second.second;for(std::size_t at=0;at<=list.size();){const auto end=list.find(u';',at);directories.push_back(list.substr(at,end==list.npos?list.size()-at:end-at));if(end==list.npos)break;at=end+1;}}}
            for(const auto& root:directories)for(const auto& candidate:names){const auto full=normalized_windows_path(candidate,root);const auto metadata=filesystem_.information(utf16_to_utf8(full));if(!metadata.error)return full;}
            throw ShellError{2};
        };
        const auto colon=file.find(u':');const bool protocol=colon!=file.npos && colon>1;
        if(protocol && (file.find_first_of(u"\\/ ")<colon || file.front()<u'A'))throw ShellError{87};
        std::u16string target=protocol?file:resolve_file(file,true),image,command;bool native_opened=false;
        const auto extension=fold(target.substr(windows_extension_offset(target)));
        const auto metadata=protocol?FileInformation{}:filesystem_.information(utf16_to_utf8(target));
        if(!protocol && class_name.empty() && !(metadata.metadata.attributes&0x10) && (extension==u".exe" || extension==u".com") && (verb.empty() || verb==u"open")){
            image=target;command=quote_argument(image)+u" "+parameters;
        }else{
            auto source=!class_name.empty()?class_name:protocol?file.substr(0,colon):(metadata.metadata.attributes&0x10)?u"Folder":extension;
            const auto association_flags=protocol && class_name.empty()?0x1000U:0U;
            if(source.empty())throw ShellError{1155};
            std::u16string unsupported;
            for(const auto kind:{7U,17U,18U})if(!query_association(association_flags,kind,source,verb,unsupported) && !unsupported.empty())throw ShellError{50};
            const auto status=query_association(association_flags,1,source,verb,command);
            // 客體未登錄瀏覽器／郵件程式時，http、https、mailto 交由宿主預設應用程式開啟；其他協定不外送到宿主。
            const auto scheme=protocol?fold(file.substr(0,colon)):std::u16string{};
            if(status==0x80070483U && !find && class_name.empty() && (verb.empty() || verb==u"open") && native_gui_ && interactive_station() &&
               (scheme==u"http" || scheme==u"https" || scheme==u"mailto")){
                const auto uri=utf16_to_utf8(file);const auto application=native_gui_->shell_default_application(nullptr,uri);
                if(!application)throw ShellError{1155};
                if(const auto error=native_gui_->shell_open(nullptr,uri,*application))throw ShellError{error};
                native_opened=true;
            }else{
                if(status)throw ShellError{status==0x80070483U?1155U:status&0xffffU};
                command=substitute(command,target,parameters);
                const auto words=parse_windows_arguments(command);if(words.empty())throw ShellError{1155};
                image=resolve_file(words.front(),true);
            }
        }
        if(native_opened){if(instance_at)memory.store(GuestAddress{instance_at},bits,42);result=extended?1:42;}
        else if(find){
            const auto bytes=wide?utf16_bytes(image):[&]{const auto text=utf16_to_utf8(image);std::vector<std::uint8_t> value(text.begin(),text.end());value.push_back(0);return value;}();
            if(bytes.size()>(wide?520U:260U))throw ShellError{206};memory.write(GuestAddress{arg(2)},bytes);result=33;
        }else{
            std::vector<std::uint8_t> buffer(128);const auto append=[&](const std::u16string& text){const auto at=buffer.size();const auto bytes=utf16_bytes(text);buffer.insert(buffer.end(),bytes.begin(),bytes.end());return at;};
            const auto app=append(image),cmd=append(command),cwd=append(directory);arena=allocate_heap(memory,buffer.size());if(!arena)throw ShellError{8};
            memory.write(GuestAddress{arena},buffer);memory.store(GuestAddress{arena},32,x86_?68:104);memory.store(GuestAddress{arena+(x86_?44:60)},32,1);memory.store(GuestAddress{arena+(x86_?48:64)},16,show);
            const std::array<std::uint64_t,10> arguments{arena+app,arena+cmd,0,0,0,(flags&0x8000)?0x08000000U:0U,0,arena+cwd,arena,arena+104};
            if(!create_guest_process(memory,arguments,true,false,0,elevated?1U:0U)){
                // Shell 的 open 也依 manifest 發起客體提權；一般 CreateProcess 保留 ERROR_ELEVATION_REQUIRED。
                if(elevated || last_error_!=740 || !create_guest_process(memory,arguments,true,false,0,1))throw ShellError{last_error_};
            }
            const auto process=memory.load(GuestAddress{arena+104},bits),thread=memory.load(GuestAddress{arena+104+unit},bits);
            (void)close_process(thread);if(extended && (flags&0x40))memory.store(GuestAddress{process_at},bits,process);else (void)close_process(process);
            if(instance_at)memory.store(GuestAddress{instance_at},bits,33);result=extended?1:33;
        }
    }catch(const ShellError& error){last_error_=error.code;const auto mapped=shell_error(error.code);if(instance_at)memory.store(GuestAddress{instance_at},bits,mapped);result=extended?0:mapped;}
    catch(const std::invalid_argument&){last_error_=123;if(instance_at)memory.store(GuestAddress{instance_at},bits,31);result=extended?0:31;}
    catch(...){if(arena)(void)free_heap(memory,arena,state);throw;}
    if(arena)(void)free_heap(memory,arena,state);arg.return_to(state,result);return {};
}
}
