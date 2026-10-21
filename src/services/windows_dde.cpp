#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>

namespace x64arm {
// DDEML：實例、字串 handle、資料 handle 與名稱服務；交談只連到同程序、同執行緒已註冊的 DDEML 伺服器，
// 依 DDEML 契約同步呼叫伺服器回呼。沒有其他 DDE 伺服器時 DdeConnect 回報 DMLERR_NO_CONV_ESTABLISHED。
struct DdeState {
    struct Instance {std::uint64_t callback{},thread{};std::uint32_t flags{},error{};bool unicode{};std::vector<std::uint64_t> services;bool filter{true};};
    struct String {std::u16string text;std::uint32_t references{};};
    struct Data {std::uint32_t instance{};std::uint64_t pointer{},size{},item{};std::uint32_t format{},flags{};};
    struct Conversation {std::uint32_t instance{};std::uint64_t partner{},service{},topic{};bool client{};};
    struct Operation {
        enum class Kind {Connect,ConnectConfirm,Transaction,Disconnect} kind{};
        std::uint32_t instance{};std::vector<std::uint32_t> candidates;std::size_t index{};
        std::uint64_t service{},topic{},context{},conversation{},data{},result_pointer{};std::uint32_t type{},format{};bool owned_data{};
    };
    std::map<std::uint32_t,Instance> instances;std::uint32_t next_instance{1};
    std::map<std::uint64_t,String> strings;std::map<std::u16string,std::uint64_t> atoms;std::uint64_t next_string{0xc000};
    std::map<std::uint64_t,Data> data;std::uint64_t next_data{0x10000};
    std::map<std::uint64_t,Conversation> conversations;std::uint64_t next_conversation{0x20000};
    std::map<std::uint64_t,Operation> operations;std::uint64_t next_operation{1};
};
namespace {
constexpr std::uint32_t dml_busy=0x4001,dml_invalid=0x4006,dml_no_conv=0x400a,dml_not_processed=0x4009,dml_memory=0x4008,dml_no_error=0;
constexpr std::uint32_t dml_dll_not_initialized=0x4003;
constexpr std::uint32_t xtyp_connect=0x1062,xtyp_connect_confirm=0x8072,xtyp_disconnect=0x80c2,xtyp_execute=0x4050,xtyp_poke=0x4090,xtyp_request=0x20b0,xtyp_advstart=0x1030,xtyp_advstop=0x8040;
std::u16string fold(std::u16string_view text){std::u16string result(text);for(auto& c:result)c=unicode_case_unit(c,false);return result;}
}
DdeState& WindowsServices::dde_state(){if(!dde_)dde_=std::make_shared<DdeState>();return *dde_;}
std::span<const WindowsServiceDescriptor> dde_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"user32.dll","DdeInitializeA",4},{"user32.dll","DdeInitializeW",4},{"user32.dll","DdeUninitialize",1},{"user32.dll","DdeGetLastError",1},
        {"user32.dll","DdeCreateStringHandleA",3},{"user32.dll","DdeCreateStringHandleW",3},{"user32.dll","DdeFreeStringHandle",2},{"user32.dll","DdeKeepStringHandle",2},
        {"user32.dll","DdeQueryStringA",5},{"user32.dll","DdeQueryStringW",5},{"user32.dll","DdeCmpStringHandles",2},
        {"user32.dll","DdeCreateDataHandle",7},{"user32.dll","DdeAddData",4},{"user32.dll","DdeGetData",4},{"user32.dll","DdeAccessData",2},
        {"user32.dll","DdeUnaccessData",1},{"user32.dll","DdeFreeDataHandle",1},
        {"user32.dll","DdeNameService",4},{"user32.dll","DdeConnect",4},{"user32.dll","DdeDisconnect",1},
        {"user32.dll","DdeClientTransaction",8},{"user32.dll","DdeQueryConvInfo",3},{"user32.dll","DdeEnableCallback",3},{"user32.dll","DdeImpersonateClient",1}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_dde(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;auto& dde=dde_state();
    const auto u32=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    const unsigned unit=x86_?4:8;
    std::uint64_t result=0;
    const auto finish=[&]{arg.return_to(state,result);return std::optional<std::uint32_t>{};};
    const auto instance_of=[&](std::uint32_t id)->DdeState::Instance*{const auto found=dde.instances.find(id);return found==dde.instances.end() || found->second.thread!=threads_.current?nullptr:&found->second;};
    const auto release_string=[&](std::uint64_t handle){
        const auto found=dde.strings.find(handle);if(found==dde.strings.end())return false;
        if(!--found->second.references){dde.atoms.erase(fold(found->second.text));dde.strings.erase(found);}return true;
    };
    const auto free_data=[&](std::uint64_t handle){
        const auto found=dde.data.find(handle);if(found==dde.data.end())return false;
        if(found->second.pointer)(void)free_heap(memory,found->second.pointer,state);dde.data.erase(found);return true;
    };
    try{
        if(symbol.starts_with("DdeInitialize")){
            if(!arg(0) || !arg(1)){result=dml_invalid;return finish();}
            memory.check(GuestAddress{arg(0)},4,Access::Write);const auto id=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)},32));
            if(arg(3)){result=dml_invalid;return finish();}
            if(id){
                // 重新初始化只更新 CBF_／APPCMD_ 旗標，不能改成另一個執行緒或回呼。
                auto* existing=instance_of(id);if(!existing || existing->callback!=arg(1) || !(u32(2)&0x80000000U)){result=dml_invalid;return finish();}
                existing->flags=u32(2)&~0x80000000U;result=dml_no_error;return finish();
            }
            if(u32(2)&0x80000000U){result=dml_invalid;return finish();}
            if(dde.instances.size()>=1024){result=dml_memory;return finish();}
            const auto created=dde.next_instance++;dde.instances.emplace(created,DdeState::Instance{arg(1),threads_.current,u32(2),0,symbol.ends_with('W'),{},true});
            memory.store(GuestAddress{arg(0)},32,created);result=dml_no_error;return finish();
        }
        if(symbol=="DdeCmpStringHandles"){
            const auto text=[&](std::uint64_t handle)->std::u16string{const auto found=dde.strings.find(handle);return found==dde.strings.end()?std::u16string{}:fold(found->second.text);};
            if(arg(0)==arg(1)){result=0;return finish();}
            if(!arg(0) || !arg(1)){result=arg(0)?1:UINT32_MAX;return finish();}
            const auto a=text(arg(0)),b=text(arg(1));result=a==b?0:a<b?UINT32_MAX:1;return finish();
        }
        if(symbol=="DdeUnaccessData"){result=dde.data.contains(arg(0))?1:0;return finish();}
        if(symbol=="DdeFreeDataHandle"){result=free_data(arg(0))?1:0;return finish();}
        if(symbol=="DdeGetData" || symbol=="DdeAccessData" || symbol=="DdeAddData"){
            const auto found=dde.data.find(arg(0));if(found==dde.data.end())return finish();auto& value=found->second;
            if(symbol=="DdeAccessData"){if(arg(1)){memory.check(GuestAddress{arg(1)},4,Access::Write);memory.store(GuestAddress{arg(1)},32,value.size);}result=value.pointer;return finish();}
            if(symbol=="DdeGetData"){
                if(u32(3)>value.size)return finish();const auto count=std::min<std::uint64_t>(value.size-u32(3),u32(2));
                if(!arg(1)){result=value.size-u32(3);return finish();}
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));memory.read(GuestAddress{value.pointer+u32(3)},bytes);memory.write(GuestAddress{arg(1)},bytes);result=count;return finish();
            }
            // DdeAddData：必要時擴大資料區塊，保留既有內容。
            const auto end=std::uint64_t{u32(3)}+u32(2);if(end>0x7fffffffU)return finish();
            if(end>value.size){const auto grown=reallocate_heap(memory,value.pointer,end,state);if(!grown)return finish();
                if(end>value.size){std::vector<std::uint8_t> zero(static_cast<std::size_t>(end-value.size));memory.write(GuestAddress{grown+value.size},zero);}
                value.pointer=grown;value.size=end;}
            if(u32(2)){std::vector<std::uint8_t> bytes(u32(2));memory.read(GuestAddress{arg(1)},bytes);memory.write(GuestAddress{value.pointer+u32(3)},bytes);}
            result=arg(0);return finish();
        }
        if(symbol=="DdeDisconnect" || symbol=="DdeQueryConvInfo" || symbol=="DdeImpersonateClient"){
            const auto found=dde.conversations.find(arg(0));
            if(found==dde.conversations.end() || !instance_of(found->second.instance))return finish();
            if(symbol=="DdeImpersonateClient"){result=found->second.client?0:1;return finish();} // 同程序客戶端與伺服器共用同一 token。
            if(symbol=="DdeQueryConvInfo"){
                // CONVINFO：cb、hUser、hConvPartner、hszSvcPartner、hszServiceReq、hszTopic、hszItem、wFmt、wType、wStatus、wConvst、wLastError、hConvList、ConvCtxt、hwnd、hwndPartner。
                if(!arg(2))return finish();memory.check(GuestAddress{arg(2)},4,Access::Read);const auto size=memory.load(GuestAddress{arg(2)},32);
                const std::uint64_t required=x86_?64:112;if(size<required)return finish();
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(required));const auto put=[&](unsigned at,std::uint64_t value,unsigned width){for(unsigned i=0;i<width;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
                const auto& conv=found->second;unsigned at=0;put(at,required,4);at=x86_?4:8;put(at,0,unit);at+=unit;put(at,conv.partner,unit);at+=unit;
                put(at,conv.service,unit);at+=unit;put(at,conv.service,unit);at+=unit;put(at,conv.topic,unit);at+=unit;at+=unit;
                put(at+8,(conv.client?1U:0U)|2U,4);put(at+12,3,4);put(at+16,instance_of(conv.instance)->error,4); // ST_CONNECTED，XST_CONNECTED。
                memory.write(GuestAddress{arg(2)},std::span(bytes).first(static_cast<std::size_t>(std::min<std::uint64_t>(required,size))));result=1;return finish();
            }
            const auto conversation=*found;const auto partner=dde.conversations.find(conversation.second.partner);
            dde.conversations.erase(found);
            if(partner==dde.conversations.end()){result=1;return finish();}
            const auto target=dde.instances.find(partner->second.instance);const auto partner_handle=partner->first;
            DdeState::Operation operation;operation.kind=DdeState::Operation::Kind::Disconnect;operation.conversation=partner_handle;
            dde.conversations.erase(partner);
            if(target==dde.instances.end() || (target->second.flags&0x00800000U)){result=1;return finish();} // CBF_SKIP_DISCONNECTS
            const auto id=dde.next_operation++;dde.operations.emplace(id,std::move(operation));
            start_callbacks(state,memory,{{target->second.callback,{xtyp_disconnect,0,partner_handle,0,0,0,0,target->first==conversation.second.instance?1U:0U}}},Continuation::DdeOperation,id);
            return std::nullopt;
        }
        if(symbol=="DdeClientTransaction"){
            const auto conversation=dde.conversations.find(arg(2));
            if(conversation==dde.conversations.end() || !conversation->second.client || !instance_of(conversation->second.instance))return finish();
            auto* client=instance_of(conversation->second.instance);
            const auto type=u32(5),format=u32(4);
            if(u32(6)==UINT32_MAX){client->error=dml_invalid;result=0;return finish();} // TIMEOUT_ASYNC 需 XTYP_XACT_COMPLETE 佇列，尚未提供。
            const auto partner=dde.conversations.find(conversation->second.partner);if(partner==dde.conversations.end()){client->error=dml_no_conv;result=0;return finish();}
            const auto server=dde.instances.find(partner->second.instance);if(server==dde.instances.end()){client->error=dml_no_conv;result=0;return finish();}
            if(arg(7)){memory.check(GuestAddress{arg(7)},4,Access::Write);memory.store(GuestAddress{arg(7)},32,0);}
            DdeState::Operation operation;operation.kind=DdeState::Operation::Kind::Transaction;operation.instance=conversation->second.instance;
            operation.conversation=arg(2);operation.type=type;operation.format=format;operation.result_pointer=arg(7);
            std::uint64_t data=0;
            if(type==xtyp_execute || type==xtyp_poke){
                if(!arg(0)){client->error=dml_invalid;result=0;return finish();}
                if(u32(1)==UINT32_MAX){data=arg(0);if(!dde.data.contains(data)){client->error=dml_invalid;result=0;return finish();}}
                else {
                    const auto pointer=allocate_heap(memory,u32(1)?u32(1):1);if(!pointer){client->error=dml_memory;result=0;return finish();}
                    std::vector<std::uint8_t> bytes(u32(1));memory.read(GuestAddress{arg(0)},bytes);memory.write(GuestAddress{pointer},bytes);
                    data=dde.next_data++;dde.data.emplace(data,DdeState::Data{conversation->second.instance,pointer,u32(1),arg(3),format,0});operation.owned_data=true;
                }
            }else if(type!=xtyp_request && type!=xtyp_advstart && type!=xtyp_advstop && (type&~0x0f00U)!=xtyp_advstart){client->error=dml_invalid;result=0;return finish();}
            operation.data=data;
            const auto id=dde.next_operation++;dde.operations.emplace(id,std::move(operation));
            start_callbacks(state,memory,{{server->second.callback,{type,format,partner->first,conversation->second.topic,arg(3),data,0,0}}},Continuation::DdeOperation,id);
            return std::nullopt;
        }
        auto* instance=instance_of(u32(0));
        if(!instance){result=symbol=="DdeGetLastError"?dml_dll_not_initialized:0;return finish();}
        const auto fail=[&](std::uint32_t error){instance->error=error;result=0;return finish();};
        if(symbol=="DdeGetLastError"){result=instance->error;instance->error=0;return finish();}
        if(symbol=="DdeUninitialize"){
            const auto id=u32(0);
            for(const auto handle:instance->services)(void)release_string(handle);
            std::erase_if(dde.conversations,[&](const auto& entry){return entry.second.instance==id;});
            for(auto it=dde.data.begin();it!=dde.data.end();){if(it->second.instance==id){if(it->second.pointer)(void)free_heap(memory,it->second.pointer,state);it=dde.data.erase(it);}else ++it;}
            dde.instances.erase(id);result=1;return finish();
        }
        if(symbol.starts_with("DdeCreateStringHandle")){
            if(!arg(1))return fail(dml_invalid);
            const auto codepage=u32(2);const bool wide=symbol.ends_with('W');
            if(codepage && codepage!=1200 && codepage!=1004)return fail(dml_invalid);
            const bool unicode=codepage==1200 || (!codepage && wide);
            auto text=unicode?read_utf16(memory,GuestAddress{arg(1)},256):utf8_to_utf16_lossy(read_ansi(memory,arg(1),512));
            if(text.size()>255)return fail(dml_invalid);
            const auto key=fold(text);
            if(const auto found=dde.atoms.find(key);found!=dde.atoms.end()){++dde.strings.at(found->second).references;result=found->second;return finish();}
            if(dde.strings.size()>=16384)return fail(dml_memory);
            const auto handle=dde.next_string++;dde.strings.emplace(handle,DdeState::String{std::move(text),1});dde.atoms.emplace(key,handle);result=handle;return finish();
        }
        if(symbol=="DdeFreeStringHandle"){result=release_string(arg(1))?1:0;if(!result)instance->error=dml_invalid;return finish();}
        if(symbol=="DdeKeepStringHandle"){const auto found=dde.strings.find(arg(1));if(found==dde.strings.end())return fail(dml_invalid);++found->second.references;result=1;return finish();}
        if(symbol.starts_with("DdeQueryString")){
            const auto found=dde.strings.find(arg(1));if(found==dde.strings.end())return fail(dml_invalid);
            const bool unicode=u32(4)==1200 || (!u32(4) && symbol.ends_with('W'));
            if(unicode){
                const auto& text=found->second.text;result=text.size();if(!arg(2))return finish();
                const auto count=std::min<std::size_t>(text.size(),u32(3)?u32(3)-1:0);std::u16string copy=text.substr(0,count);
                if(u32(3))memory.write(GuestAddress{arg(2)},utf16_bytes(copy));result=count;return finish();
            }
            const auto text=utf16_to_utf8(found->second.text);result=text.size();if(!arg(2))return finish();
            const auto count=std::min<std::size_t>(text.size(),u32(3)?u32(3)-1:0);std::vector<std::uint8_t> bytes(text.begin(),text.begin()+static_cast<std::ptrdiff_t>(count));bytes.push_back(0);
            if(u32(3))memory.write(GuestAddress{arg(2)},bytes);result=count;return finish();
        }
        if(symbol=="DdeCreateDataHandle"){
            // HDATA_APPOWNED（1）保留給建立者重複使用；其他資料在交給 DDEML 後由接收端擁有。
            const auto size=std::uint64_t{u32(2)}+u32(3);if(size>0x7fffffffU || (u32(6)&~1U))return fail(dml_invalid);
            if(dde.data.size()>=65536)return fail(dml_memory);
            const auto pointer=allocate_heap(memory,size?size:1);if(!pointer)return fail(dml_memory);
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));if(arg(1) && u32(2))memory.read(GuestAddress{arg(1)},std::span(bytes).subspan(u32(3)));
            memory.write(GuestAddress{pointer},bytes);
            const auto handle=dde.next_data++;dde.data.emplace(handle,DdeState::Data{u32(0),pointer,size,arg(4),u32(5),u32(6)});result=handle;return finish();
        }
        if(symbol=="DdeEnableCallback"){
            // EC_ENABLEALL(0)／EC_ENABLEONE(0x80)／EC_DISABLE(8)／EC_QUERYWAITING(2)：同步交易沒有排隊中的回呼。
            const auto command=u32(2);if(command!=0 && command!=0x80 && command!=8 && command!=2)return fail(dml_invalid);
            if(arg(1) && !dde.conversations.contains(arg(1)))return fail(dml_invalid);
            result=command==2?0:1;return finish();
        }
        if(symbol=="DdeNameService"){
            const auto flags=u32(3);if(arg(2) || !flags || (flags&~15U) || ((flags&1U) && (flags&2U)) || ((flags&4U) && (flags&8U)))return fail(dml_invalid);
            if(instance->flags&0x10U)return fail(dml_invalid); // APPCMD_CLIENTONLY 不能註冊伺服器名稱。
            if(flags&4U)instance->filter=true;if(flags&8U)instance->filter=false;
            if(flags&1U){if(!dde.strings.contains(arg(1)))return fail(dml_invalid);
                if(std::find(instance->services.begin(),instance->services.end(),arg(1))==instance->services.end()){++dde.strings.at(arg(1)).references;instance->services.push_back(arg(1));}}
            if(flags&2U){
                if(!arg(1)){for(const auto handle:instance->services)(void)release_string(handle);instance->services.clear();}
                else {const auto found=std::find(instance->services.begin(),instance->services.end(),arg(1));if(found==instance->services.end())return fail(dml_invalid);(void)release_string(arg(1));instance->services.erase(found);}
            }
            result=1;return finish();
        }
        if(symbol=="DdeConnect"){
            if((arg(1) && !dde.strings.contains(arg(1))) || (arg(2) && !dde.strings.contains(arg(2))))return fail(dml_invalid);
            DdeState::Operation operation;operation.kind=DdeState::Operation::Kind::Connect;operation.instance=u32(0);operation.service=arg(1);operation.topic=arg(2);operation.context=arg(3);
            for(const auto& [id,server]:dde.instances){
                if(server.thread!=threads_.current || (server.flags&0x10U) || (server.flags&0x00002000U))continue; // CLIENTONLY／CBF_FAIL_CONNECTIONS
                const bool named=!arg(1) || std::any_of(server.services.begin(),server.services.end(),[&](auto handle){return !dde.strings.contains(handle)?false:fold(dde.strings.at(handle).text)==fold(dde.strings.at(arg(1)).text);});
                if(named || (!server.filter && !server.services.empty()))operation.candidates.push_back(id);
            }
            if(operation.candidates.empty())return fail(dml_no_conv);
            if(arg(3))memory.check(GuestAddress{arg(3)},x86_?28:32,Access::Read);
            const auto server=dde.instances.at(operation.candidates.front());const auto id=dde.next_operation++;dde.operations.emplace(id,std::move(operation));
            start_callbacks(state,memory,{{server.callback,{xtyp_connect,0,0,arg(2),arg(1),0,arg(3),dde.operations.at(id).candidates.front()==u32(0)?1U:0U}}},Continuation::DdeOperation,id);
            return std::nullopt;
        }
    }catch(const MemoryFault&){if(const auto found=dde.instances.find(u32(0));found!=dde.instances.end())found->second.error=dml_invalid;result=0;return finish();}
    catch(const std::bad_alloc&){result=0;return finish();}
    throw HostCallFault("未知的 DDEML 入口");
}
std::optional<std::uint32_t> WindowsServices::resume_dde(GuestState& state,GuestMemory& memory,std::uint64_t id,std::uint64_t value){
    auto& dde=dde_state();const auto found=dde.operations.find(id);if(found==dde.operations.end()){complete_call(state,memory,0);return std::nullopt;}
    auto operation=found->second;dde.operations.erase(found);using Kind=DdeState::Operation::Kind;
    const auto mask=x86_?UINT64_C(0xffffffff):UINT64_MAX;value&=mask;
    if(operation.kind==Kind::Connect){
        const auto server_id=operation.candidates.at(operation.index);
        auto* client=dde.instances.contains(operation.instance)?&dde.instances.at(operation.instance):nullptr;
        if(value && client && dde.instances.contains(server_id)){
            // 回呼接受連線：建立成對的交談 handle，再送出 XTYP_CONNECT_CONFIRM。
            const auto client_handle=dde.next_conversation++,server_handle=dde.next_conversation++;
            dde.conversations.emplace(client_handle,DdeState::Conversation{operation.instance,server_handle,operation.service,operation.topic,true});
            dde.conversations.emplace(server_handle,DdeState::Conversation{server_id,client_handle,operation.service,operation.topic,false});
            const auto& server=dde.instances.at(server_id);
            if(server.flags&0x00040000U){complete_call(state,memory,client_handle);return std::nullopt;} // CBF_SKIP_CONNECT_CONFIRMS
            DdeState::Operation confirm;confirm.kind=Kind::ConnectConfirm;confirm.conversation=client_handle;
            const auto next=dde.next_operation++;dde.operations.emplace(next,std::move(confirm));
            start_callbacks(state,memory,{{server.callback,{xtyp_connect_confirm,0,server_handle,operation.topic,operation.service,0,0,server_id==operation.instance?1U:0U}}},Continuation::DdeOperation,next);
            return std::nullopt;
        }
        if(++operation.index<operation.candidates.size() && client){
            const auto next_server=operation.candidates[operation.index];const auto callback=dde.instances.at(next_server).callback;
            const auto next=dde.next_operation++;const auto topic=operation.topic,service=operation.service,context=operation.context;const auto same=next_server==operation.instance?1U:0U;
            dde.operations.emplace(next,std::move(operation));
            start_callbacks(state,memory,{{callback,{xtyp_connect,0,0,topic,service,0,context,same}}},Continuation::DdeOperation,next);return std::nullopt;
        }
        if(client)client->error=dml_no_conv;complete_call(state,memory,0);return std::nullopt;
    }
    if(operation.kind==Kind::ConnectConfirm){complete_call(state,memory,dde.conversations.contains(operation.conversation)?operation.conversation:0);return std::nullopt;}
    if(operation.kind==Kind::Disconnect){complete_call(state,memory,1);return std::nullopt;}
    // 交易完成：EXECUTE／POKE 依 DDE_FACK、DDE_FBUSY 判斷；REQUEST 的資料 handle 交給客戶端。
    auto* client=dde.instances.contains(operation.instance)?&dde.instances.at(operation.instance):nullptr;
    if(operation.owned_data){if(const auto data=dde.data.find(operation.data);data!=dde.data.end()){if(data->second.pointer)(void)free_heap(memory,data->second.pointer,state);dde.data.erase(data);}}
    std::uint64_t result=0;
    if(operation.type==xtyp_execute || operation.type==xtyp_poke){
        const auto status=static_cast<std::uint32_t>(value);
        if(operation.result_pointer)memory.store(GuestAddress{operation.result_pointer},32,status&0xffffU);
        if(status&0x8000U)result=1;else if(client)client->error=(status&0x4000U)?dml_busy:dml_not_processed;
    }else if(operation.type==xtyp_request){
        if(value && dde.data.contains(value)){result=value;dde.data.at(value).instance=operation.instance;}
        else if(client)client->error=dml_not_processed;
    }else if(operation.type==xtyp_advstop)result=1;
    else {result=value?1:0;if(!result && client)client->error=dml_not_processed;}
    complete_call(state,memory,result);return std::nullopt;
}
void WindowsServices::discard_dde(std::uint64_t id){if(dde_)dde_->operations.erase(id);}
}
