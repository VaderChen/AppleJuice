// ntdll 的 Rtl 登錄家族（WDK 公開語義）：RtlCheckRegistryKey、RtlCreateRegistryKey、RtlDeleteRegistryValue、RtlWriteRegistryValue、
// RtlQueryRegistryValues／Ex（查詢表：子鍵切換、必要值、預設值、NOVALUE、DELETE、直接寫入 EntryContext 與 TYPECHECK、
// REG_MULTI_SZ 逐字串回呼、REG_EXPAND_SZ 展開）、RtlOpenCurrentUser，以及 RtlpNt*Key 轉接（RtlpNtOpenKey、RtlpNtCreateKey、
// RtlpNtQueryValueKey、RtlpNtSetValueKey、RtlpNtEnumerateSubKey、RtlpNtMakeTemporaryKey）。
// 全部經既有的 Nt 登錄服務完成（同一份登錄狀態、權限與通知），RelativeTo 只決定 NT 路徑前綴。
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "service_callback_task.hpp"
#include <algorithm>

namespace x64arm {
namespace {
constexpr std::uint32_t status_invalid_parameter=0xc000000dU,status_not_found=0xc0000034U,status_path_not_found=0xc000003aU,status_buffer_too_small=0xc0000023U;
constexpr std::uint32_t status_buffer_overflow=0x80000005U,status_type_mismatch=0xc0000024U,status_no_memory=0xc0000017U;
constexpr std::uint32_t registry_handle=0x40000000U,registry_optional=0x80000000U;
constexpr std::uint32_t query_subkey=1,query_topkey=2,query_required=4,query_novalue=8,query_noexpand=0x10,query_direct=0x20,query_delete=0x40,query_typecheck=0x100;
bool failed(std::uint32_t status){return status>=0x80000000U && status!=status_buffer_overflow;}
}
std::span<const WindowsServiceDescriptor> ntdll_registry_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"ntdll.dll","RtlCheckRegistryKey",2,0,0,0x1},{"ntdll.dll","RtlCreateRegistryKey",2,0,0,0x1},{"ntdll.dll","RtlDeleteRegistryValue",3,0,0,0x1},
        {"ntdll.dll","RtlWriteRegistryValue",6,0,0,0x29},{"ntdll.dll","RtlQueryRegistryValues",5,0,0,0x1},{"ntdll.dll","RtlQueryRegistryValuesEx",5,0,0,0x1},
        {"ntdll.dll","RtlOpenCurrentUser",2,0,0,0x1},{"ntdll.dll","RtlpNtOpenKey",4,0,0,0xa},{"ntdll.dll","RtlpNtCreateKey",6,0,0,0xa},
        {"ntdll.dll","RtlpNtQueryValueKey",5,0,0,0x10},{"ntdll.dll","RtlpNtSetValueKey",4,0,0,0xa},{"ntdll.dll","RtlpNtEnumerateSubKey",4,0,0,0xc},
        {"ntdll.dll","RtlpNtMakeTemporaryKey",1}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_ntdll_registry(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    return nested_continuation(state,memory,"Rtl 登錄",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall call(s,m,service);const auto finish=[&](std::uint32_t status){call.return_to(s,status);return std::optional<std::uint32_t>{};};
        const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);const auto name=service.symbol;
        std::vector<std::uint64_t> blocks;
        struct Release {WindowsServices& self;GuestMemory& m;GuestState& s;std::vector<std::uint64_t>& blocks;~Release(){for(const auto block:blocks)(void)self.free_heap(m,block,s);}} release{*this,m,s,blocks};
        const auto scratch=[&](std::size_t size)->std::uint64_t{const auto block=allocate_heap(m,std::max<std::size_t>(size,1));if(!block)throw std::bad_alloc{};m.fill(GuestAddress{block},0,std::max<std::size_t>(size,1));blocks.push_back(block);return block;};
        const auto nt=[&](std::string_view symbol,std::initializer_list<std::uint64_t> arguments){return static_cast<std::uint32_t>(nested_service(s,m,"ntdll.dll",symbol,arguments));};
        // UNICODE_STRING（不含結尾 NUL 的長度）與 OBJECT_ATTRIBUTES（不分大小寫）。
        const auto counted=[&](std::u16string_view text){
            const auto at=scratch(2*unit+text.size()*2+2);m.write(GuestAddress{at+2*unit},utf16_bytes(text));
            m.store(GuestAddress{at},16,text.size()*2);m.store(GuestAddress{at+2},16,text.size()*2+2);m.store(GuestAddress{at+unit},bits,at+2*unit);return at;
        };
        const auto attributes=[&](std::u16string_view path,std::uint64_t root){
            const auto at=scratch(6*unit);m.store(GuestAddress{at},32,6*unit);m.store(GuestAddress{at+unit},bits,root);
            m.store(GuestAddress{at+2*unit},bits,counted(path));m.store(GuestAddress{at+3*unit},32,0x40);return at;
        };
        const auto close=[&](std::uint64_t handle){if(handle)(void)nt("NtClose",{handle});};
        // 開啟（或逐層建立）NT 路徑；回傳 {狀態, handle}。
        const std::function<std::pair<std::uint32_t,std::uint64_t>(std::u16string,bool,std::uint32_t)> open_path=[&](std::u16string path,bool create,std::uint32_t access)->std::pair<std::uint32_t,std::uint64_t>{
            while(path.size()>1 && path.back()==u'\\')path.pop_back();
            const auto out=scratch(unit);
            auto status=create?nt("NtCreateKey",{out,access,attributes(path,0),0,0,0,0}):nt("NtOpenKey",{out,access,attributes(path,0)});
            if(create && (status==status_not_found || status==status_path_not_found)){
                const auto cut=path.rfind(u'\\');if(cut==path.npos || cut==0)return {status,0};
                const auto [parent_status,parent]=open_path(path.substr(0,cut),true,0x20006);if(failed(parent_status))return {parent_status,0};close(parent);
                status=nt("NtCreateKey",{out,access,attributes(path,0),0,0,0,0});
            }
            return {status,failed(status)?0:m.load(GuestAddress{out},bits)};
        };
        // RelativeTo（ABSOLUTE／SERVICES／CONTROL／WINDOWS_NT／DEVICEMAP／USER，或 RTL_REGISTRY_HANDLE）＋ Path → NT 路徑。
        const auto user_path=[&]()->std::u16string{
            const auto out=scratch(2*unit);if(failed(nt("RtlFormatCurrentUserKeyPath",{out})))return u"\\Registry\\User\\.Default";
            const auto length=static_cast<std::size_t>(m.load(GuestAddress{out},16));const auto buffer=m.load(GuestAddress{out+unit},bits);
            std::u16string text(length/2,u'\0');for(std::size_t i=0;i<text.size();++i)text[i]=static_cast<char16_t>(m.load(GuestAddress{buffer+2*i},16));
            (void)nt("RtlFreeUnicodeString",{out});return text;
        };
        struct Opened {std::uint32_t status{};std::uint64_t handle{};bool owned{};};
        const auto open_relative=[&](std::uint32_t relative,std::uint64_t path,bool create,std::uint32_t access)->Opened{
            if(relative&registry_handle)return {0,path,false};
            const auto base=relative&~registry_optional;const auto tail=path?read_utf16(m,GuestAddress{path}):std::u16string{};
            std::u16string full;
            switch(base){
            case 0:full=tail;break;
            case 1:full=u"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\"+tail;break;
            case 2:full=u"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\"+tail;break;
            case 3:full=u"\\Registry\\Machine\\Software\\Microsoft\\Windows NT\\CurrentVersion\\"+tail;break;
            case 4:full=u"\\Registry\\Machine\\Hardware\\DeviceMap\\"+tail;break;
            case 5:full=user_path()+u"\\"+tail;break;
            default:return {status_invalid_parameter,0,false};
            }
            if(full.empty())return {status_invalid_parameter,0,false};
            const auto [status,handle]=open_path(full,create,access);return {status,handle,true};
        };
        if(name=="RtlOpenCurrentUser"){
            if(!call(1))co_return finish(status_invalid_parameter);
            const auto [status,handle]=open_path(user_path(),false,static_cast<std::uint32_t>(call(0)));
            m.store(GuestAddress{call(1)},bits,failed(status)?0:handle);co_return finish(status);
        }
        if(name=="RtlpNtOpenKey" || name=="RtlpNtCreateKey"){
            // OBJ_PERMANENT／OBJ_EXCLUSIVE 對登錄鍵沒有意義，與 Windows 相同先清除再交給 NtOpenKey／NtCreateKey。
            if(const auto object=call(2)){const auto flags=static_cast<std::uint32_t>(m.load(GuestAddress{object+3*unit},32));m.store(GuestAddress{object+3*unit},32,flags&~0x30U);}
            if(name=="RtlpNtOpenKey")co_return finish(nt("NtOpenKey",{call(0),call(1),call(2)}));
            co_return finish(nt("NtCreateKey",{call(0),call(1),call(2),0,0,0,call(5)}));
        }
        if(name=="RtlpNtMakeTemporaryKey")co_return finish(nt("NtDeleteKey",{call(0)}));
        if(name=="RtlpNtSetValueKey")co_return finish(nt("NtSetValueKey",{call(0),counted(u""),0,call(1),call(2),call(3)}));
        if(name=="RtlpNtQueryValueKey"){
            // (Key, Type*, Data, DataLength*, 保留)：查詢未命名的預設值；DataLength 為輸入容量與輸出大小。
            const auto capacity=call(3)?static_cast<std::uint32_t>(m.load(GuestAddress{call(3)},32)):0U;
            const auto size=12+capacity;const auto buffer=scratch(size),returned=scratch(4);
            const auto status=nt("NtQueryValueKey",{call(0),counted(u""),2,buffer,size,returned});
            if(failed(status) && status!=status_buffer_too_small)co_return finish(status);
            const auto length=static_cast<std::uint32_t>(m.load(GuestAddress{buffer+8},32));
            if(status==status_buffer_too_small){const auto needed=static_cast<std::uint32_t>(m.load(GuestAddress{returned},32));if(call(3))m.store(GuestAddress{call(3)},32,needed>12?needed-12:0);co_return finish(status_buffer_overflow);}
            if(call(1))m.store(GuestAddress{call(1)},32,m.load(GuestAddress{buffer+4},32));
            if(call(3))m.store(GuestAddress{call(3)},32,length);
            if(call(2) && length){std::vector<std::uint8_t> data(std::min(length,capacity));m.read(GuestAddress{buffer+12},data);m.write(GuestAddress{call(2)},data);}
            co_return finish(status==status_buffer_overflow?status_buffer_overflow:0);
        }
        if(name=="RtlpNtEnumerateSubKey"){
            // (Key, UNICODE_STRING* 名稱, Index, 保留)：名稱寫入呼叫端緩衝區，容量不足時回 STATUS_BUFFER_OVERFLOW。
            const auto target=call(1);if(!target)co_return finish(status_invalid_parameter);
            const auto buffer=scratch(16+512*2),returned=scratch(4);
            const auto status=nt("NtEnumerateKey",{call(0),static_cast<std::uint32_t>(call(2)),0,buffer,16+512*2,returned});if(failed(status))co_return finish(status);
            const auto length=static_cast<std::uint32_t>(m.load(GuestAddress{buffer+12},32));const auto maximum=static_cast<std::uint32_t>(m.load(GuestAddress{target+2},16));
            if(maximum<length)co_return finish(status_buffer_overflow);
            std::vector<std::uint8_t> text(length);m.read(GuestAddress{buffer+16},text);m.write(GuestAddress{m.load(GuestAddress{target+unit},bits)},text);
            m.store(GuestAddress{target},16,length);co_return finish(0);
        }
        const auto relative=static_cast<std::uint32_t>(call(0));
        if(name=="RtlCheckRegistryKey"){const auto key=open_relative(relative,call(1),false,0x20019);if(key.owned)close(key.handle);co_return finish(key.status);}
        if(name=="RtlCreateRegistryKey"){const auto key=open_relative(relative,call(1),true,0x2001f);if(key.owned)close(key.handle);co_return finish(key.status);}
        if(name=="RtlDeleteRegistryValue" || name=="RtlWriteRegistryValue"){
            const bool write=name=="RtlWriteRegistryValue";const auto key=open_relative(relative,call(1),write,0x20006);if(failed(key.status))co_return finish(key.status);
            const auto value=counted(call(2)?read_utf16(m,GuestAddress{call(2)}):std::u16string{});
            const auto status=write?nt("NtSetValueKey",{key.handle,value,0,call(3),call(4),call(5)}):nt("NtDeleteValueKey",{key.handle,value});
            if(key.owned)close(key.handle);co_return finish(status);
        }
        // ---- RtlQueryRegistryValues(Ex)(RelativeTo, Path, QueryTable, Context, Environment) ----
        // 查詢表含 DELETE 旗標時另需 KEY_SET_VALUE。
        const unsigned stride=7*unit;bool deletes=false;
        for(auto entry=call(2);entry;entry+=stride){
            if(!m.load(GuestAddress{entry},bits) && !m.load(GuestAddress{entry+2*unit},bits))break;
            if(m.load(GuestAddress{entry+unit},32)&query_delete)deletes=true;
        }
        const std::uint32_t access=deletes?0x2001fU:0x20019U;
        const auto base=open_relative(relative,call(1),false,access);
        if(failed(base.status))co_return finish((relative&registry_optional) && base.status==status_not_found?0U:base.status);
        std::uint64_t current=base.handle,sub=0;
        const auto context=call(3),environment=call(4);
        // 讀出值：{狀態, 型別, 資料}。
        struct Value {std::uint32_t status{},type{};std::vector<std::uint8_t> data;};
        const auto query=[&](std::uint64_t handle,std::u16string_view value_name)->Value{
            const auto value=counted(value_name),returned=scratch(4);
            auto status=nt("NtQueryValueKey",{handle,value,2,0,0,returned});
            if(status!=status_buffer_too_small && status!=status_buffer_overflow)return {status,0,{}};
            const auto size=static_cast<std::uint32_t>(m.load(GuestAddress{returned},32));const auto buffer=scratch(size);
            status=nt("NtQueryValueKey",{handle,value,2,buffer,size,returned});if(failed(status))return {status,0,{}};
            Value out;out.type=static_cast<std::uint32_t>(m.load(GuestAddress{buffer+4},32));out.data.resize(static_cast<std::size_t>(m.load(GuestAddress{buffer+8},32)));m.read(GuestAddress{buffer+12},out.data);return out;
        };
        const auto guest_bytes=[&](const std::vector<std::uint8_t>& data){const auto at=scratch(data.size()+4);if(!data.empty())m.write(GuestAddress{at},data);return at;};
        const auto expand=[&](const std::vector<std::uint8_t>& data)->std::vector<std::uint8_t>{
            std::u16string text;for(std::size_t i=0;i+1<data.size();i+=2){const auto c=static_cast<char16_t>(data[i]|(data[i+1]<<8));if(!c)break;text.push_back(c);}
            const auto source=counted(text),destination=scratch(2*unit),needed=scratch(4);
            (void)nt("RtlExpandEnvironmentStrings_U",{environment,source,destination,needed});
            const auto size=static_cast<std::uint32_t>(m.load(GuestAddress{needed},32));const auto buffer=scratch(size+2);
            m.store(GuestAddress{destination},16,0);m.store(GuestAddress{destination+2},16,std::min<std::uint32_t>(size+2,0xfffe));m.store(GuestAddress{destination+unit},bits,buffer);
            if(failed(nt("RtlExpandEnvironmentStrings_U",{environment,source,destination,needed})))return data;
            std::vector<std::uint8_t> out(static_cast<std::size_t>(m.load(GuestAddress{destination},16))+2);m.read(GuestAddress{buffer},std::span(out).first(out.size()-2));return out;
        };
        // 一筆值交給查詢表項目：直接寫入 EntryContext，或呼叫 QueryRoutine（必要時展開與分割）。
        const auto deliver=[&](std::uint64_t entry,std::u16string_view value_name,std::uint32_t type,std::vector<std::uint8_t> data)->service_callback::Task<std::uint32_t>{
            const auto routine=m.load(GuestAddress{entry},bits);const auto flags=static_cast<std::uint32_t>(m.load(GuestAddress{entry+unit},32));
            const auto entry_context=m.load(GuestAddress{entry+3*unit},bits);
            const bool text=type==1 || type==2 || type==7;
            if(type==2 && !(flags&query_noexpand)){data=expand(data);type=1;}
            if(flags&query_direct){
                if(flags&query_typecheck){const auto expected=static_cast<std::uint32_t>(m.load(GuestAddress{entry+4*unit},32))>>24;if(expected!=type)co_return status_type_mismatch;}
                if(!entry_context)co_return status_invalid_parameter;
                if(text){
                    // UNICODE_STRING：Buffer 為 NULL 時配置程序 heap；Length 不含結尾 NUL。
                    auto length=data.size();while(length>=2 && type!=7 && !data[length-1] && !data[length-2])length-=2;
                    auto buffer=m.load(GuestAddress{entry_context+unit},bits);const auto maximum=static_cast<std::uint32_t>(m.load(GuestAddress{entry_context+2},16));
                    if(!buffer){buffer=allocate_heap(m,length+2);if(!buffer)co_return status_no_memory;m.store(GuestAddress{entry_context+unit},bits,buffer);m.store(GuestAddress{entry_context+2},16,length+2);}
                    else if(maximum<length+2)co_return status_buffer_too_small;
                    if(length)m.write(GuestAddress{buffer},std::span(data).first(length));m.store(GuestAddress{buffer+length},16,0);m.store(GuestAddress{entry_context},16,length);co_return 0;
                }
                if(data.size()<=4){std::vector<std::uint8_t> word(4);std::copy(data.begin(),data.end(),word.begin());m.write(GuestAddress{entry_context},std::span(word).first(std::max<std::size_t>(data.size(),1)));co_return 0;}
                // 大於 ULONG：開頭 LONG 為緩衝區大小；負值只存資料，正值另存長度與型別。
                const auto declared=static_cast<std::int32_t>(m.load(GuestAddress{entry_context},32));const auto size=static_cast<std::uint32_t>(declared<0?-declared:declared);
                if(declared<0){if(size<data.size())co_return status_buffer_too_small;m.write(GuestAddress{entry_context},data);co_return 0;}
                if(size<data.size()+8)co_return status_buffer_too_small;
                m.store(GuestAddress{entry_context},32,data.size());m.store(GuestAddress{entry_context+4},32,type);m.write(GuestAddress{entry_context+8},data);co_return 0;
            }
            if(!routine)co_return 0;
            const auto name_at=counted(value_name)+2*unit;
            const auto invoke=[&](std::uint32_t kind,const std::vector<std::uint8_t>& bytes)->service_callback::Task<std::uint32_t>{
                const auto at=guest_bytes(bytes);co_return static_cast<std::uint32_t>(co_await service_callback::call(routine,{name_at,kind,at,bytes.size(),context,entry_context}));
            };
            if(type==7 && !(flags&query_noexpand)){
                // REG_MULTI_SZ：每個字串各回呼一次（型別 REG_SZ，長度含 NUL）。
                std::size_t start=0;bool any=false;
                while(start+1<data.size()){
                    std::size_t end=start;while(end+1<data.size() && (data[end] || data[end+1]))end+=2;
                    if(end==start)break;
                    std::vector<std::uint8_t> piece(data.begin()+static_cast<std::ptrdiff_t>(start),data.begin()+static_cast<std::ptrdiff_t>(end));piece.push_back(0);piece.push_back(0);
                    if(const auto status=co_await invoke(1,piece);failed(status))co_return status;any=true;start=end+2;
                }
                if(!any)co_return co_await invoke(1,std::vector<std::uint8_t>{0,0});
                co_return 0;
            }
            co_return co_await invoke(type,data);
        };
        std::uint32_t result=0;
        for(auto entry=call(2);entry;entry+=stride){
            const auto routine=m.load(GuestAddress{entry},bits),value_name=m.load(GuestAddress{entry+2*unit},bits);
            const auto flags=static_cast<std::uint32_t>(m.load(GuestAddress{entry+unit},32));
            if(!routine && !value_name)break;
            if(flags&(query_subkey|query_topkey)){
                if(sub){close(sub);sub=0;}current=base.handle;
                if(flags&query_subkey){
                    const auto key=value_name?read_utf16(m,GuestAddress{value_name}):std::u16string{};const auto out=scratch(unit);
                    const auto status=nt("NtOpenKey",{out,access,attributes(key,base.handle)});
                    if(failed(status)){if(flags&query_required){result=status;break;}current=0;continue;}
                    sub=current=m.load(GuestAddress{out},bits);
                }
                if(!routine)continue;
            }
            if(!current)continue;
            if(flags&query_novalue){
                if(routine){const auto name_at=value_name?value_name:counted(u"")+2*unit;
                    const auto status=static_cast<std::uint32_t>(co_await service_callback::call(routine,{name_at,0,0,0,context,m.load(GuestAddress{entry+3*unit},bits)}));if(failed(status)){result=status;break;}}
                continue;
            }
            if(!value_name || (flags&query_subkey)){
                // 沒有值名稱：列舉目前鍵的所有值。
                bool any=false;
                for(std::uint32_t index=0;;++index){
                    const auto returned=scratch(4);auto status=nt("NtEnumerateValueKey",{current,index,1,0,0,returned});
                    if(status!=status_buffer_too_small && status!=status_buffer_overflow)break;
                    const auto size=static_cast<std::uint32_t>(m.load(GuestAddress{returned},32));const auto buffer=scratch(size);
                    status=nt("NtEnumerateValueKey",{current,index,1,buffer,size,returned});if(failed(status))break;
                    const auto type=static_cast<std::uint32_t>(m.load(GuestAddress{buffer+4},32)),offset=static_cast<std::uint32_t>(m.load(GuestAddress{buffer+8},32));
                    const auto length=static_cast<std::uint32_t>(m.load(GuestAddress{buffer+12},32)),name_length=static_cast<std::uint32_t>(m.load(GuestAddress{buffer+16},32));
                    std::u16string text(name_length/2,u'\0');for(std::size_t i=0;i<text.size();++i)text[i]=static_cast<char16_t>(m.load(GuestAddress{buffer+20+2*i},16));
                    std::vector<std::uint8_t> data(length);if(length)m.read(GuestAddress{buffer+offset},data);
                    any=true;if(const auto delivered=co_await deliver(entry,text,type,std::move(data));failed(delivered)){result=delivered;break;}
                }
                if(failed(result))break;
                if(!any && (flags&query_required)){result=status_not_found;break;}
                continue;
            }
            const auto name_text=read_utf16(m,GuestAddress{value_name});auto value=query(current,name_text);
            if(failed(value.status)){
                if(flags&query_required){result=value.status==status_not_found?status_not_found:value.status;break;}
                // 預設值：DefaultType（TYPECHECK 時取低 24 位元）非 REG_NONE 才使用；字串型且長度為 0 時依 NUL 計算長度。
                const auto default_type=static_cast<std::uint32_t>(m.load(GuestAddress{entry+4*unit},32))&((flags&query_typecheck)?0xffffffU:0xffffffffU);
                const auto default_data=m.load(GuestAddress{entry+5*unit},bits);auto default_length=static_cast<std::uint32_t>(m.load(GuestAddress{entry+6*unit},32));
                if(!default_type)continue;
                if(!default_length && default_data && (default_type==1 || default_type==2))default_length=static_cast<std::uint32_t>(read_utf16(m,GuestAddress{default_data}).size()*2+2);
                if(!default_length && default_data && default_type==7){std::uint64_t at=default_data;while(m.load(GuestAddress{at},16) || m.load(GuestAddress{at+2},16))at+=2;default_length=static_cast<std::uint32_t>(at-default_data+4);}
                std::vector<std::uint8_t> data(default_length);if(default_length)m.read(GuestAddress{default_data},data);
                value={0,default_type,std::move(data)};
            }
            if(const auto delivered=co_await deliver(entry,name_text,value.type,std::move(value.data));failed(delivered)){result=delivered;break;}
            if(flags&query_delete)(void)nt("NtDeleteValueKey",{current,counted(name_text)});
        }
        if(sub)close(sub);if(base.owned)close(base.handle);
        co_return finish(result);
    });
}
}
