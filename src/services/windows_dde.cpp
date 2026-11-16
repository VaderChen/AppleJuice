#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "service_callback_task.hpp"
#include <algorithm>
#include <array>

namespace x64arm {
// DDEML：實例、字串 handle、資料 handle 與名稱服務；交談只連到同程序、同執行緒已註冊的 DDEML 伺服器，
// 依 DDEML 契約同步呼叫伺服器回呼。沒有其他 DDE 伺服器時 DdeConnect 回報 DMLERR_NO_CONV_ESTABLISHED。
struct DdeState {
    struct Instance {std::uint64_t callback{},thread{};std::uint32_t flags{},error{};bool unicode{};std::vector<std::uint64_t> services;bool filter{true};};
    struct String {std::u16string text;std::uint32_t references{};};
    struct Data {std::uint32_t instance{};std::uint64_t pointer{},size{},item{};std::uint32_t format{},flags{};};
    // 客戶端交談的 advise 連結（XTYP_ADVSTART 成功後建立，ADVSTOP 移除）：項目、格式與 XTYPF_NODATA／ACKREQ。
    struct Link {std::uint64_t item{};std::uint32_t format{},flags{};};
    struct Conversation {std::uint32_t instance{};std::uint64_t partner{},service{},topic{};bool client{};std::uint64_t user{},list{};std::vector<Link> links;std::array<std::uint8_t,36> context{};};
    struct List {std::uint32_t instance{};std::vector<std::uint64_t> conversations;};
    struct Operation {
        enum class Kind {Connect,ConnectConfirm,Transaction,Disconnect} kind{};
        std::uint32_t instance{};std::vector<std::uint32_t> candidates;std::size_t index{};
        std::uint64_t service{},topic{},context{},conversation{},data{},result_pointer{},item{};std::uint32_t type{},format{},link_flags{};bool owned_data{};
    };
    std::map<std::uint32_t,Instance> instances;std::uint32_t next_instance{1};
    std::map<std::uint64_t,String> strings;std::map<std::u16string,std::uint64_t> atoms;std::uint64_t next_string{0xc000};
    std::map<std::uint64_t,Data> data;std::uint64_t next_data{0x10000};
    std::map<std::uint64_t,Conversation> conversations;std::uint64_t next_conversation{0x20000};
    std::map<std::uint64_t,Operation> operations;std::uint64_t next_operation{1};
    std::map<std::uint64_t,List> lists;std::uint64_t next_list{0x30000};
    // DdeSetQualityOfService：視窗的 SECURITY_QUALITY_OF_SERVICE（ImpersonationLevel、ContextTrackingMode、EffectiveOnly）。
    std::map<std::uint64_t,std::array<std::uint32_t,3>> qos;
};
namespace {
constexpr std::uint32_t dml_busy=0x4001,dml_invalid=0x4006,dml_no_conv=0x400a,dml_not_processed=0x4009,dml_memory=0x4008,dml_no_error=0;
constexpr std::uint32_t dml_dll_not_initialized=0x4003;
constexpr std::uint32_t xtyp_connect=0x1062,xtyp_connect_confirm=0x8072,xtyp_disconnect=0x80c2,xtyp_execute=0x4050,xtyp_poke=0x4090,xtyp_request=0x20b0,xtyp_advstart=0x1030,xtyp_advstop=0x8040;
constexpr std::uint32_t xtyp_wildconnect=0x20e2,xtyp_advreq=0x2022,xtyp_advdata=0x4010,xtypf_nodata=0x4,dml_unfound_queue=0x4011,cbf_skip_connect_confirms=0x00040000;
std::u16string fold(std::u16string_view text){std::u16string result(text);for(auto& c:result)c=unicode_case_unit(c,false);return result;}
// DdeConnect／DdeConnectList 的伺服器候選：同執行緒、非 APPCMD_CLIENTONLY、未設 CBF_FAIL_CONNECTIONS，且註冊了要求的服務名稱（或關閉名稱過濾）。
std::vector<std::uint32_t> dde_candidates(const DdeState& dde,std::uint64_t thread,std::uint64_t service){
    std::vector<std::uint32_t> result;
    for(const auto& [id,server]:dde.instances){
        if(server.thread!=thread || (server.flags&0x10U) || (server.flags&0x00002000U))continue;
        const bool named=!service || std::any_of(server.services.begin(),server.services.end(),[&](auto handle){return !dde.strings.contains(handle) || !dde.strings.contains(service)?false:fold(dde.strings.at(handle).text)==fold(dde.strings.at(service).text);});
        if(named || (!server.filter && !server.services.empty()))result.push_back(id);
    }
    return result;
}
// 建立成對的交談 handle（客戶端、伺服器），CONVCONTEXT 為連線時的 pCC（沒有時只填 cb）。
std::uint64_t dde_create_pair(DdeState& dde,std::uint32_t client,std::uint32_t server,std::uint64_t service,std::uint64_t topic,const std::array<std::uint8_t,36>& context){
    const auto client_handle=dde.next_conversation++,server_handle=dde.next_conversation++;
    dde.conversations.emplace(client_handle,DdeState::Conversation{client,server_handle,service,topic,true,0,0,{},context});
    dde.conversations.emplace(server_handle,DdeState::Conversation{server,client_handle,service,topic,false,0,0,{},context});
    return client_handle;
}
std::array<std::uint8_t,36> dde_context(const GuestMemory& memory,std::uint64_t pointer){
    std::array<std::uint8_t,36> context{};if(pointer)memory.read(GuestAddress{pointer},context);else context[0]=36;return context;
}
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
        {"user32.dll","DdeClientTransaction",8},{"user32.dll","DdeQueryConvInfo",3},{"user32.dll","DdeEnableCallback",3},{"user32.dll","DdeImpersonateClient",1},
        {"user32.dll","DdeConnectList",5},{"user32.dll","DdeQueryNextServer",2},{"user32.dll","DdeDisconnectList",1},{"user32.dll","DdePostAdvise",3},
        {"user32.dll","DdeSetUserHandle",3,0,0,0x2},{"user32.dll","DdeAbandonTransaction",3,0,0,0x5},{"user32.dll","DdeSetQualityOfService",3},{"user32.dll","ImpersonateDdeClientWindow",2}
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
    if(symbol=="DdeConnectList" || symbol=="DdeDisconnectList" || symbol=="DdePostAdvise")return invoke_dde_lists(state,memory,service);
    try{
        if(symbol=="DdeQueryNextServer"){
            // 依序回傳清單中仍存在的下一個交談；hConvPrev 為 0 時從頭開始。
            const auto list=dde.lists.find(arg(0));if(list==dde.lists.end())return finish();
            bool passed=!arg(1);
            for(const auto conversation:list->second.conversations){if(!dde.conversations.contains(conversation))continue;if(passed){result=conversation;break;}if(conversation==arg(1))passed=true;}
            return finish();
        }
        if(symbol=="DdeSetUserHandle"){
            // QID_SYNC 設定交談本身的使用者 handle（DdeQueryConvInfo 的 hUser）；同步交易沒有非同步佇列可指定。
            const auto found=dde.conversations.find(arg(0));if(found==dde.conversations.end() || !instance_of(found->second.instance))return finish();
            if(u32(1)!=UINT32_MAX){instance_of(found->second.instance)->error=dml_unfound_queue;return finish();}
            found->second.user=arg(2);result=1;return finish();
        }
        if(symbol=="DdeAbandonTransaction"){
            // 交易都同步完成，沒有排隊中的非同步交易：idTransaction 為 0 時成功，指定編號則找不到。
            auto* owner=instance_of(u32(0));if(!owner)return finish();
            if(arg(1) && (!dde.conversations.contains(arg(1)) || dde.conversations.at(arg(1)).instance!=u32(0))){owner->error=dml_invalid;return finish();}
            if(u32(2)){owner->error=dml_unfound_queue;return finish();}
            result=1;return finish();
        }
        if(symbol=="DdeSetQualityOfService"){
            // 預設 SecurityImpersonation、SECURITY_STATIC_TRACKING、EffectiveOnly；回傳先前的設定。
            if(!gui_.windows.contains(arg(0)) || !arg(1) || memory.load(GuestAddress{arg(1)},32)!=12){last_error_=87;return finish();}
            const std::array<std::uint32_t,3> next{static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)+4},32)),static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)+8},8)),static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)+9},8))};
            if(next[0]>3){last_error_=87;return finish();}
            const auto previous=dde.qos.contains(arg(0))?dde.qos.at(arg(0)):std::array<std::uint32_t,3>{2,0,1};
            if(arg(2)){memory.store(GuestAddress{arg(2)},32,12);memory.store(GuestAddress{arg(2)+4},32,previous[0]);memory.store(GuestAddress{arg(2)+8},8,previous[1]);memory.store(GuestAddress{arg(2)+9},8,previous[2]);memory.store(GuestAddress{arg(2)+10},16,0);}
            dde.qos[arg(0)]=next;result=1;return finish();
        }
        if(symbol=="ImpersonateDdeClientWindow"){
            // 同程序的 DDE 客戶端與伺服器共用同一 token（與 DdeImpersonateClient 相同）。
            if(!gui_.windows.contains(arg(0)) || !gui_.windows.contains(arg(1))){last_error_=1400;return finish();}
            result=1;return finish();
        }
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
                // 完整結構 x86 96／x64 144 位元組；較舊、不含 hConvList 之後欄位的大小仍可接受，只寫到 cb 為止。
                if(!arg(2))return finish();memory.check(GuestAddress{arg(2)},4,Access::Read);const auto size=memory.load(GuestAddress{arg(2)},32);
                const std::uint64_t minimum=x86_?64:112,full=x86_?96:144;if(size<minimum)return finish();
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(full));const auto put=[&](unsigned at,std::uint64_t value,unsigned width){for(unsigned i=0;i<width;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
                const auto& conv=found->second;unsigned at=0;put(at,std::min<std::uint64_t>(size,full),4);at=x86_?4:8;put(at,conv.user,unit);at+=unit;put(at,conv.partner,unit);at+=unit;
                put(at,conv.service,unit);at+=unit;put(at,conv.service,unit);at+=unit;put(at,conv.topic,unit);at+=unit;at+=unit;
                put(at+8,(conv.client?1U:0U)|2U,4);put(at+12,3,4);put(at+16,instance_of(conv.instance)->error,4); // ST_CONNECTED，XST_CONNECTED。
                const unsigned list_at=x86_?48U:80U;put(list_at,conv.list,unit);std::copy(conv.context.begin(),conv.context.end(),bytes.begin()+list_at+(x86_?4:8));
                memory.write(GuestAddress{arg(2)},std::span(bytes).first(static_cast<std::size_t>(std::min<std::uint64_t>(full,size))));result=1;return finish();
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
            // XTYPF_NOBLOCK／NODATA／ACKREQ 只是 DDEML 的連結旗標，伺服器收到的是 XTYP_ADVSTART。
            const bool advise_start=(type&~0xeU)==xtyp_advstart;const auto base=advise_start?xtyp_advstart:type;
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
            }else if(type!=xtyp_request && !advise_start && type!=xtyp_advstop){client->error=dml_invalid;result=0;return finish();}
            operation.data=data;operation.item=arg(3);operation.type=base;operation.link_flags=type&0xeU;
            const auto id=dde.next_operation++;dde.operations.emplace(id,std::move(operation));
            start_callbacks(state,memory,{{server->second.callback,{base,format,partner->first,conversation->second.topic,arg(3),data,0,0}}},Continuation::DdeOperation,id);
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
            std::erase_if(dde.lists,[&](const auto& entry){return entry.second.instance==id;});
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
            operation.candidates=dde_candidates(dde,threads_.current,arg(1));
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
            const auto client_handle=dde_create_pair(dde,operation.instance,server_id,operation.service,operation.topic,dde_context(memory,operation.context));
            const auto server_handle=dde.conversations.at(client_handle).partner;
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
    }else if(operation.type==xtyp_advstop){
        result=1;
        if(const auto conv=dde.conversations.find(operation.conversation);conv!=dde.conversations.end())
            std::erase_if(conv->second.links,[&](const auto& link){return link.item==operation.item && link.format==operation.format;});
    }
    else {
        result=value?1:0;if(!result && client)client->error=dml_not_processed;
        if(result && operation.type==xtyp_advstart)if(const auto conv=dde.conversations.find(operation.conversation);conv!=dde.conversations.end()){
            auto& links=conv->second.links;const auto existing=std::find_if(links.begin(),links.end(),[&](const auto& link){return link.item==operation.item && link.format==operation.format;});
            if(existing!=links.end())existing->flags=operation.link_flags;else links.push_back({operation.item,operation.format,operation.link_flags});
        }
    }
    complete_call(state,memory,result);return std::nullopt;
}
void WindowsServices::discard_dde(std::uint64_t id){if(dde_)dde_->operations.erase(id);}
// DdeConnectList：對每個候選伺服器送 XTYP_CONNECT（服務與主題都指定時）或 XTYP_WILDCONNECT（取回 HSZPAIR 陣列），每一組建立交談並送 XTYP_CONNECT_CONFIRM；
// 沿用既有清單時略過同一伺服器、服務、主題的重複交談。DdeDisconnectList 逐一中斷清單中的交談。
// DdePostAdvise：伺服器對符合主題／項目的 advise 連結逐一送 XTYP_ADVREQ 取資料，再以 XTYP_ADVDATA 交給客戶端（XTYPF_NODATA 不取資料）；
// 非 HDATA_APPOWNED 的資料在交付後釋放。
std::optional<std::uint32_t> WindowsServices::invoke_dde_lists(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    return nested_continuation(state,memory,"DDEML 清單",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall call(s,m,service);auto& dde=dde_state();const auto symbol=service.symbol;const unsigned unit=x86_?4:8;
        const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
        const auto mask=[&](std::uint64_t value){return x86_?value&0xffffffffU:value;};
        const auto instance_of=[&](std::uint32_t id)->DdeState::Instance*{const auto found=dde.instances.find(id);return found==dde.instances.end() || found->second.thread!=threads_.current?nullptr:&found->second;};
        if(symbol=="DdeDisconnectList"){
            const auto list=dde.lists.find(call(0));if(list==dde.lists.end() || !instance_of(list->second.instance))co_return finish(0);
            const auto conversations=list->second.conversations;
            for(const auto conversation:conversations)if(dde.conversations.contains(conversation))(void)co_await service_callback::call_value(resolve("user32.dll","DdeDisconnect").value,{conversation});
            dde.lists.erase(call(0));co_return finish(1);
        }
        const auto id=static_cast<std::uint32_t>(call(0));auto* instance=instance_of(id);if(!instance)co_return finish(0);
        if(symbol=="DdePostAdvise"){
            if(instance->flags&0x10U){instance->error=dml_invalid;co_return finish(0);}
            struct Work {std::uint64_t server{},client{},topic{};DdeState::Link link;};std::vector<Work> work;
            for(const auto& [handle,conversation]:dde.conversations){
                if(conversation.instance!=id || conversation.client || (call(1) && conversation.topic!=call(1)))continue;
                const auto partner=dde.conversations.find(conversation.partner);if(partner==dde.conversations.end())continue;
                for(const auto& link:partner->second.links)if(!call(2) || link.item==call(2))work.push_back({handle,partner->first,conversation.topic,link});
            }
            for(std::size_t i=0;i<work.size();++i){
                const auto& item=work[i];
                if(!dde.conversations.contains(item.server) || !dde.conversations.contains(item.client))continue;
                const auto server=dde.instances.find(id),client=dde.instances.find(dde.conversations.at(item.client).instance);
                if(server==dde.instances.end() || client==dde.instances.end())continue;
                std::uint64_t data=0;
                if(!(item.link.flags&xtypf_nodata)){
                    data=mask(co_await service_callback::call_value(server->second.callback,{xtyp_advreq,item.link.format,item.server,item.topic,item.link.item,0,static_cast<std::uint64_t>(work.size()-i-1),0}));
                    if(!data || !dde.data.contains(data))continue;
                }
                const auto client_callback=dde.instances.at(dde.conversations.at(item.client).instance).callback;
                (void)co_await service_callback::call_value(client_callback,{xtyp_advdata,item.link.format,item.client,item.topic,item.link.item,data,0,0});
                if(data)if(const auto found=dde.data.find(data);found!=dde.data.end() && !(found->second.flags&1U)){if(found->second.pointer)(void)free_heap(m,found->second.pointer,s);dde.data.erase(found);}
            }
            co_return finish(1);
        }
        // DdeConnectList
        const auto service_name=call(1),topic=call(2),existing=call(3),context_pointer=call(4);
        if((service_name && !dde.strings.contains(service_name)) || (topic && !dde.strings.contains(topic))){instance->error=dml_invalid;co_return finish(0);}
        if(existing && (!dde.lists.contains(existing) || dde.lists.at(existing).instance!=id)){instance->error=dml_invalid;co_return finish(0);}
        if(context_pointer)m.check(GuestAddress{context_pointer},36,Access::Read);
        const auto context=dde_context(m,context_pointer);
        const auto list_handle=existing?existing:dde.next_list++;if(!existing)dde.lists.emplace(list_handle,DdeState::List{id,{}});
        for(const auto server_id:dde_candidates(dde,threads_.current,service_name)){
            if(!dde.instances.contains(server_id))continue;const auto callback=dde.instances.at(server_id).callback;const std::uint64_t same=server_id==id?1:0;
            std::vector<std::pair<std::uint64_t,std::uint64_t>> pairs;
            if(service_name && topic){
                if(mask(co_await service_callback::call_value(callback,{xtyp_connect,0,0,topic,service_name,0,context_pointer,same})))pairs.emplace_back(service_name,topic);
            }else{
                const auto data=mask(co_await service_callback::call_value(callback,{xtyp_wildconnect,0,0,topic,service_name,0,context_pointer,same}));
                if(const auto found=dde.data.find(data);data && found!=dde.data.end()){
                    for(std::uint64_t at=0;at+2*unit<=found->second.size;at+=2*unit){
                        const auto first=m.load(GuestAddress{found->second.pointer+at},static_cast<std::uint8_t>(unit*8)),second=m.load(GuestAddress{found->second.pointer+at+unit},static_cast<std::uint8_t>(unit*8));
                        if(!first && !second)break;if(dde.strings.contains(first) && dde.strings.contains(second))pairs.emplace_back(first,second);
                    }
                    if(!(found->second.flags&1U)){if(found->second.pointer)(void)free_heap(m,found->second.pointer,s);dde.data.erase(found);}
                }
            }
            for(const auto& [pair_service,pair_topic]:pairs){
                if(!dde.lists.contains(list_handle) || !dde.instances.contains(server_id))break;
                auto& list=dde.lists.at(list_handle);
                const bool duplicate=std::any_of(list.conversations.begin(),list.conversations.end(),[&](auto conversation){const auto found=dde.conversations.find(conversation);
                    return found!=dde.conversations.end() && found->second.service==pair_service && found->second.topic==pair_topic && dde.conversations.contains(found->second.partner) && dde.conversations.at(found->second.partner).instance==server_id;});
                if(duplicate)continue;
                const auto client_handle=dde_create_pair(dde,id,server_id,pair_service,pair_topic,context);
                dde.conversations.at(client_handle).list=list_handle;list.conversations.push_back(client_handle);
                const auto server_handle=dde.conversations.at(client_handle).partner;
                if(!(dde.instances.at(server_id).flags&cbf_skip_connect_confirms))
                    (void)co_await service_callback::call_value(callback,{xtyp_connect_confirm,0,server_handle,pair_topic,pair_service,0,0,same});
            }
        }
        const auto list=dde.lists.find(list_handle);
        if(list==dde.lists.end() || std::none_of(list->second.conversations.begin(),list->second.conversations.end(),[&](auto conversation){return dde.conversations.contains(conversation);})){
            if(!existing)dde.lists.erase(list_handle);if(auto* owner=instance_of(id))owner->error=dml_no_conv;co_return finish(0);
        }
        co_return finish(list_handle);
    });
}
}
