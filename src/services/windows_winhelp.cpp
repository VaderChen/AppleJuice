#include "x64arm/windows_services.hpp"
#include "x64arm/windows_paths.hpp"
#include "x64arm/unicode.hpp"

namespace x64arm {
std::optional<std::uint32_t> WindowsServices::invoke_winhelp(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);
    const auto done=[&](std::uint32_t error){if(error)last_error_=error;arg.return_to(state,error?0U:1U);return std::optional<std::uint32_t>{};};
    if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許 WinHelp 原生視窗 I/O");
    try{
        const bool localized=service.symbol=="#395" || service.symbol=="#397";
        const bool on_demand=service.symbol=="#416" || service.symbol=="#417";
        const bool wide=service.symbol.ends_with('W') || service.symbol=="#397" || service.symbol=="#416";
        if(on_demand && arg(4))return done(50);
        if(localized){
            initialize_tokens();
            if(runtime_.token_objects.at(effective_token_id()).user.sid!=runtime_.token_objects.at(1).user.sid)return done(5);
        }
        NativeHelpRequest request;request.owner=arg(0);request.command=static_cast<std::uint32_t>(arg(2));request.data=static_cast<std::uint32_t>(arg(3));
        // QUIT 不解參照已不再需要的檔名／dwData，亦允許來源視窗已被銷毀。
        if(request.command==2)return native_gui_?done(native_gui_->help(request)):done(0);
        if(request.owner && !gui_.windows.contains(request.owner))return done(1400);
        switch(request.command){case 1:case 3:case 5:case 8:case 9:case 0x101:case 0x105:break;default:return done(50);}
        if(!native_gui_)return done(50);
        const auto string=[&](std::uint64_t address){return wide?read_utf16(memory,GuestAddress{address},32768):utf8_to_utf16(read_ansi(memory,address,32768));};
        if(!arg(1))return done(87);auto file=string(arg(1));if(file.empty())return done(87);
        if(file.find(u'>')!=file.npos)return done(50); // 次要視窗須有獨立的尺寸／生命週期，尚未接入。
        if(request.command==0x101 || request.command==0x105){if(!arg(3))return done(87);request.keyword=string(arg(3));if(request.keyword.find(u';')!=request.keyword.npos)return done(50);}
        std::vector<std::u16string> candidates{normalized_windows_path(file,runtime_.current_directory)};
        if(file.find_first_of(u"\\/:")==file.npos && !runtime_.application_directory.empty())candidates.push_back(normalized_windows_path(file,runtime_.application_directory));
        if(localized){
            const auto language=nested_service(state,memory,"kernel32.dll","GetUserDefaultUILanguage",{});
            constexpr std::u16string_view digits=u"0123456789ABCDEF";
            std::u16string folder=u"mui\\0000\\";
            for(unsigned i=0;i<4;++i)folder[4+i]=digits[(language>>(12-i*4))&15];
            std::vector<std::u16string> paths;
            for(const auto& path:candidates){
                const auto leaf=path.find_last_of(u'\\')+1;
                paths.push_back(path.substr(0,leaf)+folder+path.substr(leaf));
            }
            paths.insert(paths.end(),candidates.begin(),candidates.end());
            candidates=std::move(paths);
        }
        OpenHostFile source;
        for(const auto& candidate:candidates){source=filesystem_.open_reader(utf16_to_utf8(candidate),runtime_.application_source,utf16_to_utf8(runtime_.application_directory));if(!source.error)break;}
        if(source.error)return done(source.error);
        if(source.metadata.size>UINT64_C(256)*1024*1024)return done(8);
        request.file=source.key;request.bytes.resize(static_cast<std::size_t>(source.metadata.size));const auto read=source.file->read(request.bytes,0);
        if(read.error)return done(read.error);if(read.bytes!=request.bytes.size())return done(13);
        return done(native_gui_->help(request));
    }catch(const MemoryFault&){return done(998);}catch(const std::invalid_argument&){return done(87);}catch(const std::bad_alloc&){return done(8);}
}
}
