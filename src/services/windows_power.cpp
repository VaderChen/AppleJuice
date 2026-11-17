#include "x64arm/windows_services.hpp"
#include "x64arm/windows_power.hpp"
#include "x64arm/windows_power_values.hpp"
#include "x64arm/unicode.hpp"

namespace x64arm {
struct PowerRequest {
    std::u16string reason;
    std::uint32_t execution_flags{0x80000000U};
    std::array<std::unique_ptr<NativePowerAssertion>,2> activities;
    std::array<std::uint32_t,4> counts{};
    std::array<std::unique_ptr<NativePowerAssertion>,4> assertions;
};
std::uint32_t WindowsServices::read_power_reason(GuestMemory& memory,std::uint64_t address,std::u16string& reason)const{
    if(!address){reason.clear();return 0;}
    const std::uint8_t bits=x86_?32:64;const unsigned unit=bits/8;
    try{
        memory.check(GuestAddress{address},x86_?24:32,Access::Read);
        if(memory.load(GuestAddress{address},32))return 87;const auto flags=memory.load(GuestAddress{address+4},32);
        if(flags==1){const auto pointer=memory.load(GuestAddress{address+8},bits);if(!pointer)return 87;reason=read_utf16(memory,GuestAddress{pointer});return 0;}
        if(flags!=2)return 87;
        const auto module=memory.load(GuestAddress{address+8},bits),id=memory.load(GuestAddress{address+8+unit},32),count=memory.load(GuestAddress{address+12+unit},32),strings=memory.load(GuestAddress{address+(x86_?20U:24U)},bits);
        if(id>UINT16_MAX || count>128 || (count && !strings))return 87;
        if(count)memory.check(GuestAddress{strings},static_cast<std::size_t>(count*unit),Access::Read);
        std::vector<std::u16string> arguments;
        for(std::uint64_t i=0;i<count;++i){const auto pointer=memory.load(GuestAddress{strings+i*unit},bits);if(!pointer)return 87;arguments.push_back(read_utf16(memory,GuestAddress{pointer}));}
        const auto entry=gui_resource(memory,module,(id>>4)+1,6,true);if(!entry)return 1814;const auto* image=resource_image(entry,true);if(!image)return 1814;
        auto at=image->base+memory.load(GuestAddress{entry},32);const auto size=memory.load(GuestAddress{entry+4},32),end=at+size;
        std::u16string pattern;
        for(unsigned i=0;i<16;++i){if(at>end || end-at<2)return 13;const auto length=memory.load(GuestAddress{at},16);at+=2;if(length>(end-at)/2)return 13;
            if(i==(id&15U)){for(std::uint64_t n=0;n<length;++n)pattern+=static_cast<char16_t>(memory.load(GuestAddress{at+n*2},16));break;}at+=length*2;}
        reason.clear();
        for(std::size_t i=0;i<pattern.size();++i){
            if(pattern[i]!=u'%' || i+1==pattern.size()){reason+=pattern[i];continue;}
            auto cursor=i+1;if(pattern[cursor]==u'%'){reason+=u'%';i=cursor;continue;}
            if(pattern[cursor]==u'0')break;
            if(pattern[cursor]>=u'1' && pattern[cursor]<=u'9'){
                unsigned number=0;while(cursor<pattern.size() && pattern[cursor]>=u'0' && pattern[cursor]<=u'9'){number=number*10+static_cast<unsigned>(pattern[cursor]-u'0');if(number>128)return 87;++cursor;}
                if(!number || number>arguments.size())return 87;
                if(cursor<pattern.size() && pattern[cursor]==u'!'){const auto close=pattern.find(u'!',cursor+1);if(close==pattern.npos)return 87;const auto format=pattern.substr(cursor+1,close-cursor-1);if(format!=u"s" && format!=u"ls" && format!=u"ws")return 87;cursor=close+1;}
                reason+=arguments[number-1];i=cursor-1;
            }else {const auto directive=pattern[cursor];reason+=directive==u'n'?u'\n':directive==u'r'?u'\r':directive==u't'?u'\t':directive==u'b'?u' ':directive;i=cursor;}
            if(reason.size()>32768)return 87;
        }
        return 0;
    }catch(const MemoryFault&){return 998;}catch(const HostCallFault&){return 13;}catch(const std::bad_alloc&){return 8;}
}
std::optional<std::uint32_t> WindowsServices::invoke_power(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    // 電源方案查詢與舊式方案介面集中在 windows_power_schemes.cpp，與方案寫入共用沙箱方案儲存。
    if(power_scheme_query_symbol(service.symbol))return invoke_power_schemes(state,memory,service);
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;
    std::uint64_t result=0;const std::uint8_t bits=x86_?32:64;
    const auto d=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    const auto fail=[&](std::uint32_t error){last_error_=error;result=symbol=="PowerCreateRequest"?UINT64_MAX:0;};
    if(symbol=="SetThreadExecutionState") {
        const auto flags=d(0);const bool continuous=flags&0x80000000U;
        const auto existing=thread_power_requests_.find(threads_.current);
        const auto previous=existing==thread_power_requests_.end()?0x80000000U:existing->second->execution_flags;
        if((flags&~0x80000043U) || ((flags&0x40U) && !continuous))fail(87);
        else if(flags&0x40U)fail(50); // macOS 未提供 Windows S3 Away Mode。
        else if(!runtime_.network.external_io && (flags&3U))throw HostCallFault("原生電源請求不支援 deterministic compare");
        else if(continuous && !(flags&3U)){thread_power_requests_.erase(threads_.current);result=previous;}
        else {
            auto request=std::make_shared<PowerRequest>();request->reason=u"Win64Sandbox thread execution state";
            bool valid=true;
            for(unsigned type=0;type<2;++type)if(flags&(type?1U:2U)){
                auto assertion=continuous?native_power_assertion(type,request->reason):native_power_activity(type==0);
                if(!assertion){valid=false;break;}
                if(continuous)request->assertions[type]=std::move(assertion);else request->activities[type]=std::move(assertion);
            }
            if(!valid)fail(50);
            else {
                if(continuous){request->execution_flags=flags;thread_power_requests_.insert_or_assign(threads_.current,std::move(request));}
                else if(flags&3U){
                    if(existing==thread_power_requests_.end())thread_power_requests_.emplace(threads_.current,std::move(request));
                    else for(unsigned type=0;type<2;++type)if(request->activities[type])existing->second->activities[type]=std::move(request->activities[type]);
                }
                result=previous;
            }
        }
    }else if(symbol=="SetSystemPowerState" || symbol=="SetSuspendState") {
        initialize_tokens();
        if(!runtime_.token_objects.at(effective_token_id()).has_privilege(19,true))fail(1314);
        else if(!runtime_.network.external_io)fail(5);
        else {const auto error=native_suspend_system(symbol=="SetSuspendState"?bool(arg(0)&255):!arg(0),symbol=="SetSuspendState" && (arg(2)&255));if(error)fail(error);else result=1;}
    }else if(symbol=="CallNtPowerInformation" || symbol=="GetPwrCapabilities"){
        const bool capability=symbol=="GetPwrCapabilities";
        const auto finish=[&](std::uint32_t status){
            if(capability){if(status)last_error_=status==0xc0000005U?998:status==0xc0000017U?8:status==0xc000000dU?87:50;arg.return_to(state,status?0:1);}
            else arg.return_to(state,status);
            return std::optional<std::uint32_t>{};
        };
        const auto level=capability?4U:d(0);const auto output=arg(capability?0:3);
        const auto required=level==4?76U:level==5?32U:level==11?runtime_.processor_count*24U:level==12?16U:0U;
        if(!required)return finish(0xc00000bbU);
        if(!capability && (arg(1) || d(2)))return finish(0xc000000dU);
        if(!capability && d(4)<required)return finish(0xc0000023U);
        if(!output)return finish(0xc000000dU);
        try{
            memory.check(GuestAddress{output},required,Access::Write);
            if(!runtime_.network.external_io)return finish(0xc00000bbU);
            if(level==4){const auto value=native_power_capabilities();if(!value)return finish(0xc00000bbU);memory.write(GuestAddress{output},*value);}
            else if(level==5){const auto value=native_power_battery_state();if(!value)return finish(0xc00000bbU);memory.write(GuestAddress{output},*value);}
            else if(level==12){const auto value=native_system_power_information();if(!value)return finish(0xc00000bbU);memory.write(GuestAddress{output},*value);}
            else {
                const auto value=native_processor_power_information(runtime_.processor_count);
                if(!value || value->size()!=runtime_.processor_count)return finish(0xc00000bbU);
                for(std::size_t i=0;i<value->size();++i)for(unsigned field=0;field<6;++field)
                    memory.store(GuestAddress{output+i*24+field*4},32,(*value)[i][field]);
            }
            return finish(0);
        }catch(const MemoryFault&){return finish(0xc0000005U);}catch(const std::bad_alloc&){return finish(0xc0000017U);}
    }else if(symbol=="GetDevicePowerState"){
        // 沙箱磁碟與裝置皆由宿主檔案系統承載；可開啟的檔案或裝置 handle 即處於 D0（開啟）。
        if(!files_.contains(arg(0)))fail(6);else if(!arg(1))fail(87);else {memory.store(GuestAddress{arg(1)},32,1);result=1;}
    }else if(symbol=="PowerDeterminePlatformRole" || symbol=="PowerDeterminePlatformRoleEx"){
        const auto version=symbol.ends_with("Ex")?static_cast<std::uint32_t>(arg(0)):1U;
        if(version!=1 && version!=2)last_error_=87;
        else {
            if(!runtime_.network.external_io)throw HostCallFault("原生電源平台角色不支援 deterministic compare");
            result=native_power_role().value_or(0);
        }
    }else if(symbol=="PowerCreateRequest"){
        if(!arg(0))fail(87);
        else {std::u16string reason;const auto error=read_power_reason(memory,arg(0),reason);
            if(error)fail(error);else if(power_requests_.size()>=1024)fail(8);
            else {
                auto request=std::make_shared<PowerRequest>();
                request->reason=std::move(reason);
                result=runtime_.allocate_wait_handle();power_requests_.emplace(result,std::move(request));
            }
        }
    }else if(symbol=="DuplicateHandle"){
        const auto source=power_requests_.find(arg(1));const bool closing=(d(6)&1)!=0;
        const bool protected_source=(runtime_.handle_flags[arg(1)]&2)!=0;
        // DUPLICATE_CLOSE_SOURCE 即使複製失敗仍關閉來源；先保留物件到完成檢查。
        const auto object=source==power_requests_.end()?std::shared_ptr<PowerRequest>{}:source->second;
        if(!object || !current_process_handle(arg(0),0x40))fail(6);
        else if(protected_source && closing)fail(5);
        else if((d(6)&~3U) || (!arg(2) && !closing))fail(87);
        else if(!arg(2))result=1;
        else if(!current_process_handle(arg(2),0x40))fail(5);
        else if(!arg(3))fail(87);
        else if(!(d(6)&2U))fail(50); // POWER_REQUEST 沒有公開細分的存取權契約。
        else if(power_requests_.size()>=1024)fail(8);
        else {
            memory.check(GuestAddress{arg(3)},x86_?4:8,Access::Write);
            const auto handle=runtime_.allocate_wait_handle();power_requests_.emplace(handle,object);
            runtime_.handle_flags[handle]=arg(5)?1:0;
            memory.store(GuestAddress{arg(3)},bits,handle);result=1;
        }
        if(object && closing && !protected_source && current_process_handle(arg(0),0x40)){
            power_requests_.erase(arg(1));runtime_.handle_flags.erase(arg(1));
        }
    }else {
        const auto found=power_requests_.find(arg(0));const auto type=d(1);
        if(found==power_requests_.end())fail(6);
        else if(type>3)fail(87);
        else if(type==2)fail(50); // macOS 沒有 Windows S3 Away Mode。
        else {
            auto& request=*found->second;auto& count=request.counts[type];
            if(symbol=="PowerSetRequest"){
                if(count==UINT32_MAX)fail(534);
                else if(count){++count;result=1;}
                else {
                    if(!runtime_.network.external_io)throw HostCallFault("原生電源請求不支援 deterministic compare");
                    auto assertion=native_power_assertion(type,request.reason);
                    if(!assertion)fail(50);else {request.assertions[type]=std::move(assertion);count=1;result=1;}
                }
            }else if(symbol=="PowerClearRequest"){
                if(!count)fail(87);else {if(!--count)request.assertions[type].reset();result=1;}
            }else throw HostCallFault("未知的電源請求服務");
        }
    }
    arg.return_to(state,result);return std::nullopt;
}
}
