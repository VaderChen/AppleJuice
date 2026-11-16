// keyed event 家族（ntdll）：NtCreateKeyedEvent、NtOpenKeyedEvent、NtReleaseKeyedEvent、NtWaitForKeyedEvent（Zw* 為同一實作）。
// 物件身分由同名的事件物件承載（NtCreateEvent／NtOpenEvent 的名稱、權限與 handle 生命期），NULL handle 為系統的
// \KernelObjects\CritSecOutOfMemoryEvent。配對狀態依「物件身分＋鍵」保存：釋放者遇到等待者即喚醒一個並返回，否則等待直到有等待者
// 取用（或逾時）；等待者遇到已在等待的釋放者即與其配對。阻塞以私有事件經 WaitForSingleObjectEx 完成（可警示、逾時）。
// 配對狀態只在本程序內，跨程序的同名 keyed event 不互相配對。
#include "x64arm/windows_services.hpp"
#include "service_callback_task.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_paths.hpp"
#include <algorithm>

namespace x64arm {
std::span<const WindowsServiceDescriptor> keyed_event_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"ntdll.dll","NtCreateKeyedEvent",4,0,0,0xa},{"ntdll.dll","ZwCreateKeyedEvent",4,0,0,0xa},{"ntdll.dll","NtOpenKeyedEvent",3,0,0,0x2},{"ntdll.dll","ZwOpenKeyedEvent",3,0,0,0x2},
        {"ntdll.dll","NtReleaseKeyedEvent",4,0,0,0x4},{"ntdll.dll","ZwReleaseKeyedEvent",4,0,0,0x4},{"ntdll.dll","NtWaitForKeyedEvent",4,0,0,0x4},{"ntdll.dll","ZwWaitForKeyedEvent",4,0,0,0x4}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_keyed_event(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    return nested_continuation(state,memory,"keyed event",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall call(s,m,service);const auto finish=[&](std::uint32_t status){call.return_to(s,status);return std::optional<std::uint32_t>{};};
        const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);auto name=service.symbol;name.remove_prefix(2); // Nt*／Zw* 同一實作
        const auto entry=[&](std::string_view dll,std::string_view symbol){return resolve(dll,symbol).value;};
        // OBJECT_ATTRIBUTES 的名稱（RootDirectory 時以 handle 前綴區分），作為程序內的物件身分。
        const auto object_name=[&](std::uint64_t attributes)->std::u16string{
            if(!attributes)return {};const auto text=m.load(GuestAddress{attributes+2*unit},bits);if(!text)return {};
            const auto length=static_cast<std::size_t>(m.load(GuestAddress{text},16));const auto buffer=m.load(GuestAddress{text+unit},bits);
            std::u16string out(length/2,u'\0');for(std::size_t i=0;i<out.size();++i)out[i]=windows_path_upper(static_cast<char16_t>(m.load(GuestAddress{buffer+2*i},16)));
            const auto root=m.load(GuestAddress{attributes+unit},bits);if(root)out=u"#"+utf8_to_utf16(std::to_string(root))+u"\\"+out;return out;
        };
        if(name=="CreateKeyedEvent" || name=="OpenKeyedEvent"){
            // 以同名事件承載物件：NtCreateEvent（手動重設、未觸發）／NtOpenEvent，權限與名稱衝突沿用事件物件的判定。
            const auto out=call(0);if(!out)co_return finish(0xc000000dU);
            const bool create=name=="CreateKeyedEvent";if(create && static_cast<std::uint32_t>(call(3)))co_return finish(0xc000000dU);
            const auto access=(static_cast<std::uint32_t>(call(1))&0xffff0000U)|0x1f0003U;
            const auto status=static_cast<std::uint32_t>(create?co_await service_callback::call(entry("ntdll.dll","NtCreateEvent"),{out,access,call(2),0,0})
                :co_await service_callback::call(entry("ntdll.dll","NtOpenEvent"),{out,access,call(2)}));
            if(status>=0x80000000U)co_return finish(status);
            const auto handle=m.load(GuestAddress{out},bits);auto identity=object_name(call(2));
            if(identity.empty())identity=u"@"+utf8_to_utf16(std::to_string(handle));
            keyed_event_handles_[handle]=identity;co_return finish(status);
        }
        // Release／Wait(Handle, Key, Alertable, Timeout)：鍵的最低位元必須為 0。
        const auto handle=call(0),key=call(1);const bool alertable=static_cast<std::uint8_t>(call(2))!=0;
        if(key&1U)co_return finish(0xc00000f0U); // STATUS_INVALID_PARAMETER_1
        std::u16string identity=u"\\KERNELOBJECTS\\CRITSECOUTOFMEMORYEVENT";
        if(handle){const auto found=keyed_event_handles_.find(handle);if(found==keyed_event_handles_.end())co_return finish(0xc0000008U);identity=found->second;}
        const bool release=name=="ReleaseKeyedEvent";
        auto& queues=keyed_events_[identity][key];
        // 先找相反方向、尚未配對的參與者。
        auto& opposite=release?queues.waiters:queues.releasers;
        if(const auto partner=std::find_if(opposite.begin(),opposite.end(),[](const auto& item){return !item.paired;});partner!=opposite.end()){
            partner->paired=true;const auto event=partner->event;opposite.erase(partner);
            (void)nested_service(s,m,"kernel32.dll","SetEvent",{event});co_return finish(0);
        }
        std::uint32_t milliseconds=UINT32_MAX;
        if(const auto timeout=call(3)){
            const auto value=static_cast<std::int64_t>(m.load(GuestAddress{timeout},64));std::uint64_t ticks=0;
            if(value<0)ticks=static_cast<std::uint64_t>(-value);else {const auto now=runtime_.system_filetime();ticks=static_cast<std::uint64_t>(value)>now?static_cast<std::uint64_t>(value)-now:0;}
            milliseconds=static_cast<std::uint32_t>(std::min<std::uint64_t>(0xfffffffeU,(ticks+9999)/10000));
        }
        const auto event=nested_service(s,m,"kernel32.dll","CreateEventW",{0,1,0,0});if(!event)co_return finish(0xc000009aU);
        (release?queues.releasers:queues.waiters).push_back({event,false});
        const auto result=static_cast<std::uint32_t>(co_await service_callback::call(entry("kernel32.dll","WaitForSingleObjectEx"),{event,milliseconds,alertable?1U:0U}));
        // 喚醒後判定：已配對即成功；逾時或 APC 時撤回自己的登記（若期間恰被配對仍視為成功）。
        auto& again=keyed_events_[identity][key];auto& mine=release?again.releasers:again.waiters;
        const auto self=std::find_if(mine.begin(),mine.end(),[&](const auto& item){return item.event==event;});
        const bool paired=self==mine.end();if(!paired)mine.erase(self);
        if(again.releasers.empty() && again.waiters.empty())keyed_events_[identity].erase(key);
        (void)nested_service(s,m,"kernel32.dll","CloseHandle",{event});
        if(paired)co_return finish(0);
        co_return finish(result==0xc0U?0xc0U:0x102U); // STATUS_USER_APC 或 STATUS_TIMEOUT
    });
}
}
