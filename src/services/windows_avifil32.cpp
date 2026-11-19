#include "x64arm/windows_services.hpp"
#include "x64arm/windows_guid.hpp"
#include "x64arm/unicode.hpp"
#include "service_callback_task.hpp"
#include "x64arm/registry_store.hpp"
#include "registry_text_view.hpp"
#include "dialog_template.hpp"
#include <algorithm>
#include <set>

namespace x64arm {
namespace {
constexpr std::uint32_t bad_parameter=0x8004406a, bad_handle=0x8004406c, unsupported=0x80044065;

using FilterPair=std::pair<std::u16string,std::u16string>;

std::vector<FilterPair> avi_filters(const RegistryTextView& registry){
    const std::u16string extensions=u"AVIFile\\Extensions";
    std::set<std::u16string> names;
    for(const auto& root:registry.roots){
        const auto prefix=registry.path_key(root+u"\\"+extensions)+u"\\";
        const auto& keys=registry.registry.keys;
        for(auto it=keys.lower_bound(prefix);it!=keys.end() && it->first.starts_with(prefix);++it){
            const auto child=it->first.substr(prefix.size());
            if(!child.empty() && child.find(u'\\')==child.npos)names.insert(child);
        }
    }
    std::map<std::u16string,std::size_t> groups;
    std::vector<FilterPair> filters{{u"All multimedia files",u""}};
    for(const auto& name:names){
        const auto cls=registry.value(extensions+u"\\"+name);
        if(!cls || cls->empty())continue;
        const auto description=registry.value(u"CLSID\\"+*cls);
        if(!description || description->empty())continue;
        const auto [group,added]=groups.emplace(registry_text_fold(*cls),filters.size());
        if(added)filters.emplace_back(*description,u"");
        for(auto index:{std::size_t{0},group->second}){
            auto& pattern=filters[index].second;
            if(!pattern.empty())pattern+=u';';
            pattern+=u"*."+name;
        }
    }
    filters.emplace_back(u"All files (*.*)",u"*.*");
    return filters;
}

}
std::span<const WindowsServiceDescriptor> avifil32_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"avifil32.dll","AVIBuildFilter",3},{"avifil32.dll","AVIBuildFilterA",3},{"avifil32.dll","AVIBuildFilterW",3},
        {"avifil32.dll","DllRegisterServer",0},{"avifil32.dll","DllUnregisterServer",0},
        {"avifil32.dll","DllGetClassObject",3},{"avifil32.dll","DllCanUnloadNow",0},
#include "avi_disk_vtables.inc"
        {"avifil32.dll","AVIClearClipboard",0},{"avifil32.dll","AVIGetFromClipboard",1},
        {"avifil32.dll","AVIMakeStreamFromClipboard",3},{"avifil32.dll","AVIPutFileOnClipboard",1},
        {"avifil32.dll","CreateEditableStream",2},
        {"avifil32.dll","EditStreamCut",4},
        {"avifil32.dll","EditStreamCopy",4},
        {"avifil32.dll","EditStreamPaste",6},
        {"avifil32.dll","EditStreamClone",2},
        {"avifil32.dll","EditStreamSetInfo",3},
        {"avifil32.dll","EditStreamSetInfoA",3},
        {"avifil32.dll","EditStreamSetInfoW",3},
        {"avifil32.dll","EditStreamSetName",2},
        {"avifil32.dll","EditStreamSetNameA",2},
        {"avifil32.dll","EditStreamSetNameW",2},

        {"avifil32.dll","AVISaveOptions",5},{"avifil32.dll","AVISaveOptionsFree",2},{"internal-avifil32-options","DialogProc",4},
        {"avifil32.dll","AVISave",6,0,0,8,true},{"avifil32.dll","AVISaveA",6,0,0,8,true},{"avifil32.dll","AVISaveW",6,0,0,8,true},
        {"avifil32.dll","AVISaveV",6},{"avifil32.dll","AVISaveVA",6},{"avifil32.dll","AVISaveVW",6},
        {"avifil32.dll","AVIMakeFileFromStreams",3},{"avifil32.dll","AVIMakeCompressedStream",4},{"avifil32.dll","AVIStreamGetFrameOpen",2},
        {"internal-avi-frame","QueryInterface",3},
        {"internal-avi-frame","AddRef",1},
        {"internal-avi-frame","Release",1},
        {"internal-avi-frame","GetFrame",2},
        {"internal-avi-frame","Begin",4},
        {"internal-avi-frame","End",1},
        {"internal-avi-frame","SetFormat",7},

        {"avifil32.dll","AVIFileInit",0},{"avifil32.dll","AVIFileExit",0},
        {"avifil32.dll","AVIFileOpen",4},{"avifil32.dll","AVIFileOpenA",4},{"avifil32.dll","AVIFileOpenW",4},
        {"avifil32.dll","AVIStreamOpenFromFile",6},{"avifil32.dll","AVIStreamOpenFromFileA",6},{"avifil32.dll","AVIStreamOpenFromFileW",6},
        {"avifil32.dll","AVIStreamCreate",4},{"avifil32.dll","AVIStreamBeginStreaming",4},{"avifil32.dll","AVIStreamEndStreaming",1},
        {"avifil32.dll","AVIStreamStart",1},{"avifil32.dll","AVIStreamLength",1},
        {"avifil32.dll","AVIStreamTimeToSample",2},{"avifil32.dll","AVIStreamSampleToTime",2}
    };
    return table;
}

std::optional<std::uint32_t> WindowsServices::invoke_avifil32(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const auto name=service.symbol;
#include "windows_avi_registration.inc"
#include "windows_avi_options_proc.inc"
    if(name.ends_with("Clipboard") || service.family.starts_with("internal-avi-") || name=="CreateEditableStream" || name.starts_with("EditStream") || name=="AVIMakeFileFromStreams" || name=="AVIStreamGetFrameOpen" || name=="DllGetClassObject" || name=="DllCanUnloadNow")return invoke_avi_disk(state,memory,service);
    if(name.starts_with("AVIBuildFilter")){
        const WindowsCall args(state,memory,service);
        const auto done=[&](std::uint32_t hr){args.return_to(state,hr);return std::optional<std::uint32_t>{};};
        const auto output=args(0);
        const auto capacity=static_cast<std::int32_t>(args(1));
        if(!output)return done(bad_parameter);
        if(capacity<2)return done(0x8004406bU);
        const bool wide=name.ends_with('W');
        if(!wide)memory.store(GuestAddress{output},16,0);
        if(!registry_file_.empty() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許持久登錄檔 I/O");
        std::vector<FilterPair> filters;
        {
            RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);
            if(transaction.error)return done(0x800440c7U);
            initialize_tokens();
            RegistryTextView registry{runtime_.registry,class_registry_roots(runtime_.registry,
                token_user_sid_string(effective_token_id()),token_user_sid_string(1)),registry_view(0,x86_)};
            if(!registry.exists(u"AVIFile\\Extensions") || !registry.exists(u"CLSID"))return done(0x800440c7U);
            filters=avi_filters(registry);
        }
        std::u16string data;
        for(std::size_t index=0;index<filters.size();++index){
            const auto& [description,pattern]=filters[index];
            const auto used=data.size();
            data+=description;data.push_back(0);
            data+=pattern;data.push_back(0);
            const bool last=index+1==filters.size();
            if(data.size()+1+last>static_cast<unsigned>(capacity)){
                if(wide){
                    data.resize(used);
                    data.push_back(0);
                    if(!last)data.push_back(0);
                    memory.write(GuestAddress{output},std::span(reinterpret_cast<const std::uint8_t*>(data.data()),2*data.size()));
                }
                return done(0x80044074U);
            }
        }
        data.push_back(0);
        const auto bytes=std::span(reinterpret_cast<const std::uint8_t*>(data.data()),2*data.size());
        if(wide)memory.write(GuestAddress{output},bytes);
        else{
            GuestScratch scratch(*this,memory,state);
            const auto text=scratch.allocate(bytes.size());
            if(!text)return done(0x80044067U);
            memory.write(GuestAddress{text},bytes);
            const auto convert=[&](std::uint64_t dest,std::uint64_t size){
                return nested_service(state,memory,"kernel32.dll","WideCharToMultiByte",{0,0,text,data.size(),dest,size,0,0});
            };
            const auto size=convert(0,0);
            if(!size || size>static_cast<unsigned>(capacity))return done(0x80044074U);
            if(!convert(output,size))return done(0x800440c7U);
        }
        return done(0);
    }
    if(name=="AVIFileInit" || name=="AVIFileExit"){
        const WindowsCall args(state,memory,service);
        if(name=="AVIFileInit"){
            const auto status=nested_service(state,memory,"ole32.dll","OleInitialize",{0});
            if(static_cast<std::uint32_t>(status)<=1)++threads_.items.at(threads_.current).avi_initializations;
        }else if(auto& refs=threads_.items.at(threads_.current).avi_initializations;refs){
            --refs;state.rip=resolve("ole32.dll","OleUninitialize");return std::nullopt;
        }
        args.return_to(state,0);return std::nullopt;
    }
    return nested_continuation(state,memory,"AVIFile",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall args(s,m,service);
        const auto symbol=service.symbol;
        const auto finish=[&](std::uint64_t hr){args.return_to(s,hr);return std::optional<std::uint32_t>{};};
        const auto bits=static_cast<std::uint8_t>(x86_?32:64);
        const auto ptr=[&](std::uint64_t p){return m.load(GuestAddress{p},bits);};
        const auto method=[&](std::uint64_t p,unsigned slot){return ptr(ptr(p)+slot*(bits/8));};
        const auto failed=[](std::uint64_t hr){return static_cast<std::uint32_t>(hr)&0x80000000U;};
        GuestScratch scratch(*this,m,s);
        const auto temp=scratch.allocate(208);
        if(!temp)co_return finish(0x80044067U);
        m.write(GuestAddress{temp},std::array<std::uint8_t,208>{});
        const auto output=args(0);
#include "windows_avi_options.inc"
#include "windows_avi_save.inc"
        if(symbol=="AVIStreamBeginStreaming" || symbol=="AVIStreamEndStreaming"){
            if(!output)co_return finish(bad_handle);
            m.write(GuestAddress{temp+16},ole_standard_guid(0x20022));
            const auto qi=co_await service_callback::call(method(output,0),{output,temp+16,temp});
            const auto streaming=ptr(temp);
            if(failed(qi) || !streaming)co_return finish(0);
            std::uint64_t hr=0;
            if(symbol=="AVIStreamBeginStreaming")
                hr=co_await service_callback::call(method(streaming,3),{streaming,args(1),args(2),args(3)});
            else (void)co_await service_callback::call(method(streaming,4),{streaming});
            (void)co_await service_callback::call(method(streaming,2),{streaming});
            co_return finish(hr);
        }
        const bool start=symbol=="AVIStreamStart",length=symbol=="AVIStreamLength",to_time=symbol=="AVIStreamSampleToTime";
        if(start || length || to_time || symbol=="AVIStreamTimeToSample"){
            const auto error=start || length?0U:UINT32_MAX;
            if(!output)co_return finish(error);
            const auto input=start || length?0:static_cast<std::int32_t>(args(1));
            if(!start && !length && !to_time && input<0)co_return finish(error);
            const auto hr=co_await service_callback::call(method(output,4),{output,temp,204});
            if(failed(hr))co_return finish(error);
            const auto field=[&](unsigned offset){return m.load(GuestAddress{temp+offset},32);};
            const auto first=field(28),count=field(32);
            if(start || length)co_return finish(start?first:count);
            const auto scale=field(20),rate=field(24);
            if(!scale || !rate)co_return finish(error);
            const auto scaled=scale*1000;
            const auto numerator=to_time?scaled:rate,denominator=to_time?rate:scaled;
            const auto position=to_time?std::clamp<std::uint64_t>(static_cast<std::uint32_t>(std::max(input,0)),first,first+count):static_cast<std::uint64_t>(input);
            const auto converted=(static_cast<unsigned __int128>(position)*numerator+(rate/scale>=1000?denominator-1:0))/denominator;
            if(to_time)co_return finish(converted>INT32_MAX?UINT32_MAX:static_cast<std::uint32_t>(converted));
            co_return finish(static_cast<std::uint32_t>(std::clamp<unsigned __int128>(converted,first,first+count)));
        }
        const bool create=symbol=="AVIStreamCreate";
        if(!output || (!create && !args(1)))co_return finish(bad_parameter);
        m.store(GuestAddress{output},bits,0);
        if(symbol.starts_with("AVIStreamOpenFromFile")){
            const auto open=resolve("avifil32.dll",symbol.ends_with('W')?"AVIFileOpenW":"AVIFileOpenA");
            auto hr=co_await service_callback::call(open.value,{temp,args(1),args(4),args(5)});
            const auto file=ptr(temp);
            if(failed(hr) || !file)co_return finish(hr);
            hr=co_await service_callback::call(method(file,4),{file,output,args(2),args(3)});
            (void)co_await service_callback::call(method(file,2),{file});
            co_return finish(hr);
        }
        auto clsid=args(3);
        if(create && !clsid)co_return finish(unsupported);
        auto path=create?0:args(1);
        if(!create && !symbol.ends_with('W')){
            const auto size=static_cast<std::uint32_t>(nested_service(s,m,"kernel32.dll","MultiByteToWideChar",{0,0,path,UINT32_MAX,0,0}));
            if(!size)co_return finish(bad_parameter);
            path=scratch.allocate(2ULL*size);
            if(!path)co_return finish(0x80044067U);
            if(!nested_service(s,m,"kernel32.dll","MultiByteToWideChar",{0,0,args(1),UINT32_MAX,path,size}))co_return finish(bad_parameter);
        }
        if(!clsid){
            auto class_id=ole_standard_guid(0x20000);
            const auto filename=read_utf16(m,GuestAddress{path});
            const auto ext=filename.find_last_of(u'.');
            if(ext!=std::u16string::npos){
                const auto value=class_registry_value(u"AVIFile\\Extensions\\"+filename.substr(ext+1,3),u"");
                if(value)class_id=parse_windows_guid(*value,true).value_or(class_id);
            }
            clsid=temp+32;m.write(GuestAddress{clsid},class_id);
        }
        m.write(GuestAddress{temp+16},ole_standard_guid(create?0x20021:0x20020));
        auto hr=co_await service_callback::call(resolve("ole32.dll","CoCreateInstance").value,{clsid,0,3,temp+16,temp});
        const auto object=ptr(temp);
        if(failed(hr) || !object)co_return finish(hr);
        if(create){
            hr=co_await service_callback::call(method(object,3),{object,static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(args(1)))),static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(args(2))))});
        }else{
            m.write(GuestAddress{temp+16},ole_standard_guid(0x10b));
            hr=co_await service_callback::call(method(object,0),{object,temp+16,temp+8});
            const auto persist=ptr(temp+8);
            if(!failed(hr) && persist){
                auto mode=static_cast<std::uint32_t>(args(2));if((mode&0x1000) && !(mode&3))mode|=1;
                hr=co_await service_callback::call(method(persist,5),{persist,path,mode});
                (void)co_await service_callback::call(method(persist,2),{persist});
            }else if(!failed(hr))hr=0x80004002U;
        }
        if(failed(hr))(void)co_await service_callback::call(method(object,2),{object});
        else m.store(GuestAddress{output},bits,object);
        co_return finish(hr);
    });
}
}
