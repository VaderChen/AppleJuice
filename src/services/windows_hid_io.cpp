#include "x64arm/windows_services.hpp"
#include "x64arm/windows_raw_input.hpp"
#include "x64arm/windows_security_attributes.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>

namespace x64arm {
namespace {
std::u16string folded(std::u16string_view text){std::u16string out(text);for(auto& c:out)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);return out;}
}
bool hid_path(std::u16string_view text){return folded(text).starts_with(u"\\\\?\\hid#macwin_");}
std::optional<std::uint32_t> WindowsServices::open_hid(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service,const std::u16string& path,const FileOpenArguments* arguments){
    const WindowsCall frame(state,memory,service);const auto arg=[&](unsigned i){return arguments?arguments->at(i):frame(i);};const auto d=[&](unsigned n){return static_cast<std::uint32_t>(arg(n));};std::uint64_t result=x86_?UINT32_MAX:UINT64_MAX;
    const auto fail=[&](std::uint32_t error){last_error_=error;};
    try {
        const auto security=synchronization_security(memory,arg(3),x86_);
        if(d(4)!=3 || arg(6) || (d(1)&~0xc0000003U) || (d(2)&~3U) || (d(5)&~0x40000080U))fail(87);
        else if(security.error)fail(security.error);
        else if(!runtime_.network.external_io)fail(50);
        else if(files_.size()>=1024)fail(4);
        else {
            prepare_input_backend();if(!raw_input_)raw_input_=std::make_shared<RawInputState>();poll_dinput();
            const auto name=folded(path);const auto found=std::find_if(raw_input_->devices.begin(),raw_input_->devices.end(),[&](const auto& item){return item.second.device.attached && folded(item.second.name)==name;});
            if(found==raw_input_->devices.end())fail(2);
            else if(found->second.preparsed.empty())fail(50);
            else {
                const auto category=[](std::uint32_t access){return ((access&0x80000001U)?1U:0U)|((access&0x40000002U)?2U:0U);};
                const bool conflict=std::any_of(files_.begin(),files_.end(),[&](const auto& item){return item.second.hid && item.second.hid->device.instance==found->second.device.instance && ((category(d(1))&~item.second.sharing) || (category(item.second.access)&~d(2)));});
                if(conflict)fail(32);
                else {FileHandle file;file.hid=std::make_shared<HidFile>();file.hid->device=found->second.device;file.hid->backend=input_backend();file.hid->layout=hid::decode(found->second.preparsed);
                    file.bytes=std::make_shared<const std::vector<std::uint8_t>>();file.key=utf16_to_utf8(found->second.name);file.path=file.key;file.access=d(1);file.sharing=d(2);file.mode=(d(5)&0x40000000U)?0U:0x20U;
                    const auto handle=runtime_.allocate_wait_handle();file.object=handle;files_.emplace(handle,std::move(file));runtime_.handle_flags[handle]=security.inherit?1U:0U;result=handle;}
            }
        }
    }catch(const MemoryFault&){fail(998);}catch(const hid::Error&){fail(13);}catch(const std::bad_alloc&){fail(8);}
    frame.return_to(state,result);return std::nullopt;
}
PipeResult WindowsServices::poll_hid_io(PendingFileIo& io,GuestMemory& memory){
    const auto file=files_.find(io.handle);if(file==files_.end() || !file->second.hid)return {0,995};auto& device=*file->second.hid;
    if(device.disconnected || !device.backend->available(device.device.instance))return {0,1167};
    try {
        if(io.kind==PendingFileIo::Kind::Flush){device.reports.clear();return {};}
        if(io.kind==PendingFileIo::Kind::Read){
            if(!device.layout.bytes(0))return {0,50};
            if(io.output_size<device.layout.bytes(0))return {0,122};if(device.reports.empty())return {0,0,true};
            const auto& report=device.reports.front();memory.write(GuestAddress{io.output},report);const auto size=static_cast<std::uint32_t>(report.size());device.reports.pop_front();return {size,0};
        }
        if(io.kind==PendingFileIo::Kind::Write){
            if(io.input_size>output_limit)return {0,87};std::vector<std::uint8_t> report(io.input_size);memory.read(GuestAddress{io.input},report);device.layout.validate(1,report);
            const auto error=device.backend->set_report(device.device.instance,1,report);return {error?0U:io.input_size,error};
        }
        return {0,1};
    }catch(const MemoryFault&){return {0,998};}catch(const hid::Error&){return {0,87};}catch(const std::bad_alloc&){return {0,8};}
}
}
