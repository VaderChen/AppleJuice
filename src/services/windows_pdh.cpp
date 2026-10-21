#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/processor_information.hpp"
#include "x64arm/system_performance.hpp"
#include "x64arm/windows_power.hpp"
#include <algorithm>
#include <cmath>

namespace x64arm {
// PDH 即時計數器：Processor／Processor Information／Memory 物件以虛擬 vCPU 為實例，
// 取樣來源與 GetSystemTimes、CallNtPowerInformation、GetPerformanceInfo 相同。
namespace pdh {
constexpr std::uint32_t no_machine=0x800007D0U,more_data=0x800007D2U,no_data=0x800007D5U,no_instance=0x800007D1U;
constexpr std::uint32_t no_object=0xC0000BB8U,no_counter=0xC0000BB9U,invalid_cstatus=0xC0000BBAU,memory_failure=0xC0000BBBU;
constexpr std::uint32_t invalid_handle=0xC0000BBCU,invalid_argument=0xC0000BBDU,bad_name=0xC0000BC0U,invalid_data=0xC0000BC6U,not_implemented=0xC0000BD3U;
enum class Object {Processor,ProcessorInformation,Memory};
enum class Counter {ProcessorTime,IdleTime,UserTime,PrivilegedTime,Frequency,Performance,Utility,MaximumFrequency,
    AvailableBytes,AvailableKBytes,AvailableMBytes,CommittedBytes,CommitLimit,CommittedInUse};
struct CounterName {Object object;std::u16string_view name;Counter counter;std::uint32_t type;bool rate,percent;};
constexpr std::uint32_t timer=0x20510500U,timer_inverse=0x21510500U,raw=0x00010000U,large_raw=0x00010100U,bulk=0x40020500U,fraction=0x20020400U;
constexpr CounterName counter_names[]{
    {Object::Processor,u"% processor time",Counter::ProcessorTime,timer_inverse,true,true},
    {Object::Processor,u"% idle time",Counter::IdleTime,timer,true,true},
    {Object::Processor,u"% user time",Counter::UserTime,timer,true,true},
    {Object::Processor,u"% privileged time",Counter::PrivilegedTime,timer,true,true},
    {Object::ProcessorInformation,u"% processor time",Counter::ProcessorTime,timer_inverse,true,true},
    {Object::ProcessorInformation,u"% idle time",Counter::IdleTime,timer,true,true},
    {Object::ProcessorInformation,u"% user time",Counter::UserTime,timer,true,true},
    {Object::ProcessorInformation,u"% privileged time",Counter::PrivilegedTime,timer,true,true},
    {Object::ProcessorInformation,u"processor frequency",Counter::Frequency,raw,false,false},
    {Object::ProcessorInformation,u"% processor performance",Counter::Performance,bulk,true,false},
    {Object::ProcessorInformation,u"% processor utility",Counter::Utility,bulk,true,true},
    {Object::ProcessorInformation,u"% of maximum frequency",Counter::MaximumFrequency,raw,false,true},
    {Object::Memory,u"available bytes",Counter::AvailableBytes,large_raw,false,false},
    {Object::Memory,u"available kbytes",Counter::AvailableKBytes,large_raw,false,false},
    {Object::Memory,u"available mbytes",Counter::AvailableMBytes,large_raw,false,false},
    {Object::Memory,u"committed bytes",Counter::CommittedBytes,large_raw,false,false},
    {Object::Memory,u"commit limit",Counter::CommitLimit,large_raw,false,false},
    {Object::Memory,u"% committed bytes in use",Counter::CommittedInUse,fraction,false,true},
};
struct Snapshot {
    std::vector<SystemCpuTime> cpus;SystemCpuTime total;
    std::vector<std::array<std::uint32_t,6>> power;std::uint32_t nominal_mhz{};
    SystemPerformance memory;
};
struct Path {Object object{};const CounterName* counter{};std::u16string instance;bool wildcard{},has_instance{};};
struct CounterState {std::uint64_t query{},user{};Path path;std::shared_ptr<const Snapshot> previous,current;};
struct Query {std::uint64_t user{};std::vector<std::uint64_t> counters;};
std::u16string fold(std::u16string_view text){std::u16string result(text);for(auto& c:result)c=unicode_case_unit(c,false);return result;}
std::uint32_t parse(std::u16string_view text,Path& out){
    if(text.starts_with(u"\\\\")){
        const auto end=text.find(u'\\',2);if(end==text.npos)return bad_name;
        const auto machine=fold(text.substr(2,end-2));
        if(machine!=u"." && machine!=u"localhost" && machine!=u"x64arm")return no_machine;
        text.remove_prefix(end);
    }
    if(!text.starts_with(u"\\"))return bad_name;text.remove_prefix(1);
    const auto separator=text.rfind(u'\\');if(separator==text.npos || separator+1==text.size())return bad_name;
    auto object=text.substr(0,separator);const auto counter=fold(text.substr(separator+1));
    if(const auto open=object.find(u'(');open!=object.npos){
        if(!object.ends_with(u')'))return bad_name;
        out.instance=std::u16string(object.substr(open+1,object.size()-open-2));out.has_instance=true;object=object.substr(0,open);
        if(out.instance.empty())return bad_name;
        if(const auto index=out.instance.find(u'#');index!=out.instance.npos){
            // 同名實例索引：本實例集合沒有重名，#0 等同無索引。
            if(out.instance.substr(index+1)!=u"0")out.instance=u"\x01";else out.instance.resize(index);
        }
        out.wildcard=out.instance==u"*";
    }
    const auto name=fold(object);
    if(name==u"processor")out.object=Object::Processor;
    else if(name==u"processor information")out.object=Object::ProcessorInformation;
    else if(name==u"memory")out.object=Object::Memory;
    else return no_object;
    if(out.object==Object::Memory && out.has_instance)return bad_name;
    if(out.object!=Object::Memory && !out.has_instance)return bad_name;
    for(const auto& item:counter_names)if(item.object==out.object && item.name==counter){out.counter=&item;return 0;}
    return no_counter;
}
// 英文顯示名稱與 Perflib\009 的標準索引（0 表示該名稱沒有固定索引）。
struct DisplayName {Object object;Counter counter;std::u16string_view name;std::uint32_t index;};
constexpr std::u16string_view object_names[]{u"Processor",u"Processor Information",u"Memory"};
constexpr DisplayName display_names[]{
    {Object::Processor,Counter::ProcessorTime,u"% Processor Time",6},{Object::Processor,Counter::IdleTime,u"% Idle Time",0},
    {Object::Processor,Counter::UserTime,u"% User Time",142},{Object::Processor,Counter::PrivilegedTime,u"% Privileged Time",144},
    {Object::ProcessorInformation,Counter::ProcessorTime,u"% Processor Time",6},{Object::ProcessorInformation,Counter::IdleTime,u"% Idle Time",0},
    {Object::ProcessorInformation,Counter::UserTime,u"% User Time",142},{Object::ProcessorInformation,Counter::PrivilegedTime,u"% Privileged Time",144},
    {Object::ProcessorInformation,Counter::Frequency,u"Processor Frequency",0},{Object::ProcessorInformation,Counter::Performance,u"% Processor Performance",0},
    {Object::ProcessorInformation,Counter::Utility,u"% Processor Utility",0},{Object::ProcessorInformation,Counter::MaximumFrequency,u"% of Maximum Frequency",0},
    {Object::Memory,Counter::AvailableBytes,u"Available Bytes",24},{Object::Memory,Counter::AvailableKBytes,u"Available KBytes",0},
    {Object::Memory,Counter::AvailableMBytes,u"Available MBytes",0},{Object::Memory,Counter::CommittedBytes,u"Committed Bytes",26},
    {Object::Memory,Counter::CommitLimit,u"Commit Limit",30},{Object::Memory,Counter::CommittedInUse,u"% Committed Bytes In Use",0}};
// 經查證的 Windows 標準索引；System 物件本身沒有提供計數器，但名稱查詢與 Windows 一致。
constexpr std::pair<std::uint32_t,std::u16string_view> standard_indexes[]{{2,u"System"},{4,u"Memory"},{6,u"% Processor Time"},{24,u"Available Bytes"},{26,u"Committed Bytes"},
    {30,u"Commit Limit"},{142,u"% User Time"},{144,u"% Privileged Time"},{238,u"Processor"}};
std::vector<std::u16string> instances(Object object,unsigned count){
    std::vector<std::u16string> result;
    if(object==Object::Memory){result.emplace_back();return result;}
    for(unsigned i=0;i<count;++i)result.push_back((object==Object::ProcessorInformation?u"0,":u"")+utf8_to_utf16(std::to_string(i)));
    if(object==Object::ProcessorInformation)result.push_back(u"0,_Total");
    result.push_back(u"_Total");return result;
}
// 回傳 std::nullopt 代表此實例沒有可用資料；error 指出應回報的 CStatus。
std::optional<double> value(const CounterState& counter,std::size_t index,unsigned count,std::uint32_t& error){
    const auto& current=*counter.current;const auto cpu=std::min<std::size_t>(index,count);
    const auto times=[&](const Snapshot& snapshot){return cpu<count?snapshot.cpus.at(cpu):snapshot.total;};
    const auto megahertz=[&](const Snapshot& snapshot,unsigned field)->double{
        if(snapshot.power.empty())return -1;
        if(cpu<count)return snapshot.power.at(cpu)[field];
        double sum=0;for(const auto& item:snapshot.power)sum+=item[field];return sum/static_cast<double>(snapshot.power.size());
    };
    const auto& name=*counter.path.counter;error=invalid_cstatus;
    if(name.object==Object::Memory){
        if(current.memory.error)return std::nullopt;const auto& pages=current.memory.pages;
        switch(name.counter){
        case Counter::AvailableBytes:return static_cast<double>(pages[4])*4096;
        case Counter::AvailableKBytes:return static_cast<double>(pages[4])*4;
        case Counter::AvailableMBytes:return std::floor(static_cast<double>(pages[4])*4096/1048576);
        case Counter::CommittedBytes:return static_cast<double>(pages[0])*4096;
        case Counter::CommitLimit:return static_cast<double>(pages[1])*4096;
        default:return pages[1]?100.0*static_cast<double>(pages[0])/static_cast<double>(pages[1]):0.0;
        }
    }
    if(name.counter==Counter::Frequency || name.counter==Counter::MaximumFrequency){
        const auto now=megahertz(current,2),maximum=megahertz(current,1);if(now<0)return std::nullopt;
        if(name.counter==Counter::Frequency)return now;return maximum>0?100.0*now/maximum:0.0;
    }
    if(!counter.previous)return std::nullopt;
    const auto before=times(*counter.previous),after=times(current);
    const auto delta=[](std::uint64_t a,std::uint64_t b){return b>=a?static_cast<double>(b-a):0.0;};
    const auto total=delta(before.kernel+before.user,after.kernel+after.user),idle=delta(before.idle,after.idle);
    const auto busy=total>idle?total-idle:0.0;
    const auto performance=[&]()->double{
        const auto now=megahertz(current,2);if(now<0)return -1;
        const double base=current.nominal_mhz?current.nominal_mhz:megahertz(current,1);return base>0?100.0*now/base:-1;
    };
    switch(name.counter){
    case Counter::ProcessorTime:return total>0?100.0*busy/total:0.0;
    case Counter::IdleTime:return total>0?100.0*std::min(idle,total)/total:0.0;
    case Counter::UserTime:return total>0?100.0*delta(before.user,after.user)/total:0.0;
    case Counter::PrivilegedTime:{const auto kernel=delta(before.kernel,after.kernel);return total>0?100.0*(kernel>idle?kernel-idle:0.0)/total:0.0;}
    case Counter::Performance:{const auto p=performance();if(p<0)return std::nullopt;return p;}
    default:{const auto p=performance();if(p<0)return std::nullopt;return total>0?busy/total*p:0.0;}
    }
}
}
struct PdhState {std::map<std::uint64_t,pdh::Query> queries;std::map<std::uint64_t,pdh::CounterState> counters;};
std::span<const WindowsServiceDescriptor> pdh_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"pdh.dll","PdhOpenQueryA",3},{"pdh.dll","PdhOpenQueryW",3},{"pdh.dll","PdhOpenQuery",3},
        {"pdh.dll","PdhAddCounterA",4},{"pdh.dll","PdhAddCounterW",4},{"pdh.dll","PdhAddEnglishCounterA",4},{"pdh.dll","PdhAddEnglishCounterW",4},
        {"pdh.dll","PdhRemoveCounter",1},{"pdh.dll","PdhCollectQueryData",1},{"pdh.dll","PdhCloseQuery",1},
        {"pdh.dll","PdhGetFormattedCounterValue",4},{"pdh.dll","PdhGetFormattedCounterArrayA",5},{"pdh.dll","PdhGetFormattedCounterArrayW",5},
        {"pdh.dll","PdhValidatePathA",1},{"pdh.dll","PdhValidatePathW",1},
        {"pdh.dll","PdhEnumObjectsA",6},{"pdh.dll","PdhEnumObjectsW",6},{"pdh.dll","PdhEnumObjectItemsA",9},{"pdh.dll","PdhEnumObjectItemsW",9},
        {"pdh.dll","PdhConnectMachineA",1},{"pdh.dll","PdhConnectMachineW",1},{"pdh.dll","PdhEnumMachinesA",3},{"pdh.dll","PdhEnumMachinesW",3},
        {"pdh.dll","PdhLookupPerfNameByIndexA",4},{"pdh.dll","PdhLookupPerfNameByIndexW",4},{"pdh.dll","PdhLookupPerfIndexByNameA",3},{"pdh.dll","PdhLookupPerfIndexByNameW",3},
        {"pdh.dll","PdhGetRawCounterValue",3}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_pdh(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;
    const auto finish=[&](std::uint32_t status){arg.return_to(state,status);return std::optional<std::uint32_t>{};};
    if(!pdh_)pdh_=std::make_shared<PdhState>();auto& pdh_state=*pdh_;const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    const unsigned count=std::max(1U,runtime_.processor_count);
    const bool wide=symbol.ends_with('W');
    const auto text=[&](unsigned i){return wide?read_utf16(memory,GuestAddress{arg(i)},1024):utf8_to_utf16_lossy(read_ansi(memory,arg(i),2048));};
    const auto text_at=[&](std::uint64_t pointer){return wide?read_utf16(memory,GuestAddress{pointer},1024):utf8_to_utf16_lossy(read_ansi(memory,pointer,2048));};
    try{
        if(symbol.starts_with("PdhOpenQuery")){
            if(!arg(2))return finish(pdh::invalid_argument);memory.check(GuestAddress{arg(2)},unit,Access::Write);
            if(arg(0))return finish(pdh::not_implemented); // 記錄檔資料來源尚未提供，只支援即時資料。
            if(pdh_state.queries.size()>=4096)return finish(pdh::memory_failure);
            const auto handle=runtime_.allocate_wait_handle();pdh_state.queries.emplace(handle,pdh::Query{arg(1),{}});
            memory.store(GuestAddress{arg(2)},bits,handle);return finish(0);
        }
        if(symbol.starts_with("PdhValidatePath")){
            if(!arg(0))return finish(pdh::invalid_argument);pdh::Path path;return finish(pdh::parse(text(0),path));
        }
        if(symbol.starts_with("PdhAdd")){
            const auto query=pdh_state.queries.find(arg(0));if(query==pdh_state.queries.end())return finish(pdh::invalid_handle);
            if(!arg(1) || !arg(3))return finish(pdh::invalid_argument);memory.check(GuestAddress{arg(3)},unit,Access::Write);
            pdh::Path path;if(const auto error=pdh::parse(text(1),path))return finish(error);
            if(pdh_state.counters.size()>=65536)return finish(pdh::memory_failure);
            const auto handle=runtime_.allocate_wait_handle();pdh_state.counters.emplace(handle,pdh::CounterState{arg(0),arg(2),std::move(path),{},{}});
            query->second.counters.push_back(handle);memory.store(GuestAddress{arg(3)},bits,handle);return finish(0);
        }
        if(symbol=="PdhRemoveCounter"){
            const auto counter=pdh_state.counters.find(arg(0));if(counter==pdh_state.counters.end())return finish(pdh::invalid_handle);
            auto& owners=pdh_state.queries.at(counter->second.query).counters;std::erase(owners,arg(0));pdh_state.counters.erase(counter);return finish(0);
        }
        if(symbol=="PdhCloseQuery"){
            const auto query=pdh_state.queries.find(arg(0));if(query==pdh_state.queries.end())return finish(pdh::invalid_handle);
            for(const auto handle:query->second.counters)pdh_state.counters.erase(handle);pdh_state.queries.erase(query);return finish(0);
        }
        if(symbol=="PdhCollectQueryData"){
            const auto query=pdh_state.queries.find(arg(0));if(query==pdh_state.queries.end())return finish(pdh::invalid_handle);
            if(query->second.counters.empty())return finish(pdh::no_data);
            auto snapshot=std::make_shared<pdh::Snapshot>();
            if(runtime_.host_clock){
                const auto host=native_processor_cpu_times();
                snapshot->cpus.assign(count,{});
                if(!host.empty()){
                    // 宿主核心依序分配到客體 vCPU；宿主核心較少時各 vCPU 共用對應核心。
                    if(host.size()>=count)for(std::size_t i=0;i<host.size();++i){auto& target=snapshot->cpus[i*count/host.size()];target.idle+=host[i].idle;target.kernel+=host[i].kernel;target.user+=host[i].user;}
                    else for(unsigned i=0;i<count;++i)snapshot->cpus[i]=host[i%host.size()];
                }
            }else {
                // 虛擬時鐘：一個 retired instruction 為一個 tick，與 GetSystemTimes 相同，平均分攤到各 vCPU。
                std::uint64_t user=0;for(const auto& [id,thread]:threads_.items){(void)id;user+=thread.cycles;}
                const auto elapsed=runtime_.clock_ticks*count;user=std::min(user,elapsed);
                for(unsigned i=0;i<count;++i){const auto share=user/count,idle=(elapsed-user)/count;snapshot->cpus.push_back({idle,idle,share,0});}
            }
            for(const auto& cpu:snapshot->cpus){snapshot->total.idle+=cpu.idle;snapshot->total.kernel+=cpu.kernel;snapshot->total.user+=cpu.user;}
            if(runtime_.network.external_io){
                if(const auto power=native_processor_power_information(count);power && power->size()==count)snapshot->power=*power;
                snapshot->nominal_mhz=native_processor_information().nominal_mhz;
                snapshot->memory=native_system_performance();
            }else snapshot->memory.error=50;
            for(const auto handle:query->second.counters){auto& counter=pdh_state.counters.at(handle);counter.previous=std::move(counter.current);counter.current=snapshot;}
            return finish(0);
        }
        // 多字串輸出：A 版以 ANSI（UTF-8 子集）編碼，W 版 UTF-16；長度單位為字元。
        const auto multi=[&](const std::vector<std::u16string>& items){std::u16string list;for(const auto& item:items){list+=item;list.push_back(0);}list.push_back(0);return list;};
        const auto store_list=[&](std::uint64_t buffer,std::uint64_t length_at,const std::u16string& list)->bool{
            const auto encoded=utf16_to_utf8(list);const auto length=wide?list.size():encoded.size();const auto capacity=memory.load(GuestAddress{length_at},32);
            memory.store(GuestAddress{length_at},32,length);if(!buffer || capacity<length)return false;
            if(wide)memory.write(GuestAddress{buffer},utf16_bytes(list));else memory.write(GuestAddress{buffer},std::span(reinterpret_cast<const std::uint8_t*>(encoded.data()),encoded.size()));return true;
        };
        const auto local_machine=[&](std::uint64_t pointer){if(!pointer)return true;auto name=pdh::fold(text_at(pointer));if(name.starts_with(u"\\\\"))name.erase(0,2);return name.empty() || name==u"." || name==u"localhost" || name==u"x64arm";};
        if(symbol.starts_with("PdhConnectMachine"))return finish(local_machine(arg(0))?0:pdh::no_machine);
        if(symbol.starts_with("PdhEnumMachines")){if(arg(0))return finish(pdh::not_implemented);if(!arg(2))return finish(pdh::invalid_argument);return finish(store_list(arg(1),arg(2),multi({u"\\\\X64ARM"}))?0:pdh::more_data);}
        if(symbol.starts_with("PdhEnumObjects") && !symbol.starts_with("PdhEnumObjectItems")){
            if(arg(0))return finish(pdh::not_implemented);if(!local_machine(arg(1)))return finish(pdh::no_machine);if(!arg(3))return finish(pdh::invalid_argument);
            std::vector<std::u16string> names(std::begin(pdh::object_names),std::end(pdh::object_names));return finish(store_list(arg(2),arg(3),multi(names))?0:pdh::more_data);
        }
        if(symbol.starts_with("PdhEnumObjectItems")){
            if(arg(0))return finish(pdh::not_implemented);if(!local_machine(arg(1)))return finish(pdh::no_machine);if(!arg(2) || !arg(4) || !arg(6))return finish(pdh::invalid_argument);
            const auto object=pdh::fold(text_at(arg(2)));std::size_t found=std::size(pdh::object_names);
            for(std::size_t i=0;i<std::size(pdh::object_names);++i)if(pdh::fold(pdh::object_names[i])==object)found=i;
            if(found==std::size(pdh::object_names))return finish(pdh::no_object);const auto kind_object=static_cast<pdh::Object>(found);
            std::vector<std::u16string> counters;for(const auto& item:pdh::display_names)if(item.object==kind_object)counters.emplace_back(item.name);
            std::vector<std::u16string> objects;if(kind_object!=pdh::Object::Memory)objects=pdh::instances(kind_object,count);
            const bool a=store_list(arg(3),arg(4),multi(counters));const bool b=kind_object==pdh::Object::Memory?(memory.store(GuestAddress{arg(6)},32,0),true):store_list(arg(5),arg(6),multi(objects));
            return finish(a && b?0:pdh::more_data);
        }
        if(symbol.starts_with("PdhLookupPerfNameByIndex")){
            if(!local_machine(arg(0)))return finish(pdh::no_machine);if(!arg(3))return finish(pdh::invalid_argument);
            const auto index=static_cast<std::uint32_t>(arg(1));const auto found=std::find_if(std::begin(pdh::standard_indexes),std::end(pdh::standard_indexes),[&](const auto& item){return item.first==index;});
            if(found==std::end(pdh::standard_indexes))return finish(pdh::invalid_argument);
            std::u16string name(found->second);const auto encoded=utf16_to_utf8(name);const auto length=(wide?name.size():encoded.size())+1;const auto capacity=memory.load(GuestAddress{arg(3)},32);
            memory.store(GuestAddress{arg(3)},32,length);if(!arg(2) || capacity<length)return finish(pdh::more_data);
            if(wide)memory.write(GuestAddress{arg(2)},utf16_bytes(name));else{memory.write(GuestAddress{arg(2)},std::span(reinterpret_cast<const std::uint8_t*>(encoded.data()),encoded.size()));memory.store(GuestAddress{arg(2)+encoded.size()},8,0);}
            return finish(0);
        }
        if(symbol.starts_with("PdhLookupPerfIndexByName")){
            if(!local_machine(arg(0)))return finish(pdh::no_machine);if(!arg(1) || !arg(2))return finish(pdh::invalid_argument);const auto name=pdh::fold(text_at(arg(1)));
            const auto found=std::find_if(std::begin(pdh::standard_indexes),std::end(pdh::standard_indexes),[&](const auto& item){return pdh::fold(item.second)==name;});
            if(found==std::end(pdh::standard_indexes))return finish(pdh::invalid_argument);memory.store(GuestAddress{arg(2)},32,found->first);return finish(0);
        }
        const auto counter=pdh_state.counters.find(arg(0));if(counter==pdh_state.counters.end())return finish(pdh::invalid_handle);
        if(symbol=="PdhGetRawCounterValue"){
            // PDH_RAW_COUNTER：CStatus、TimeStamp(FILETIME)、FirstValue、SecondValue、MultiCount。計時型計數器以 100 ns CPU 時間為值與時基。
            if(!arg(2))return finish(pdh::invalid_argument);memory.check(GuestAddress{arg(2)},40,Access::Write);memory.fill(GuestAddress{arg(2)},0,40);
            const auto& counter_name=*counter->second.path.counter;if(arg(1))memory.store(GuestAddress{arg(1)},32,counter_name.type);
            if(!counter->second.current){memory.store(GuestAddress{arg(2)},32,pdh::invalid_cstatus);return finish(pdh::invalid_data);}
            const auto names=pdh::instances(counter->second.path.object,count);std::size_t index=0;
            if(counter->second.path.object!=pdh::Object::Memory){index=names.size();for(std::size_t i=0;i<names.size();++i)if(pdh::fold(names[i])==pdh::fold(counter->second.path.instance)){index=i;break;}}
            if(index==names.size()){memory.store(GuestAddress{arg(2)},32,pdh::no_instance);return finish(pdh::invalid_data);}
            const auto& current=*counter->second.current;std::int64_t first=0,second=0;
            if(counter_name.rate){const auto cpu=std::min<std::size_t>(index,count);const auto& times=cpu<count?current.cpus.at(cpu):current.total;
                second=static_cast<std::int64_t>(times.kernel+times.user);
                switch(counter_name.counter){case pdh::Counter::ProcessorTime:case pdh::Counter::IdleTime:first=static_cast<std::int64_t>(times.idle);break;
                    case pdh::Counter::UserTime:first=static_cast<std::int64_t>(times.user);break;case pdh::Counter::PrivilegedTime:first=static_cast<std::int64_t>(times.kernel>times.idle?times.kernel-times.idle:0);break;
                    default:{std::uint32_t status=0;const auto value=pdh::value(counter->second,index,count,status);first=value?static_cast<std::int64_t>(*value):0;second=100;break;}}
            }else {std::uint32_t status=0;const auto value=pdh::value(counter->second,index,count,status);if(!value){memory.store(GuestAddress{arg(2)},32,status);return finish(pdh::invalid_data);}first=static_cast<std::int64_t>(*value);}
            const auto now=runtime_.system_time_state().filetime;memory.store(GuestAddress{arg(2)+4},32,static_cast<std::uint32_t>(now));memory.store(GuestAddress{arg(2)+8},32,static_cast<std::uint32_t>(now>>32));
            memory.store(GuestAddress{arg(2)+16},64,static_cast<std::uint64_t>(first));memory.store(GuestAddress{arg(2)+24},64,static_cast<std::uint64_t>(second));memory.store(GuestAddress{arg(2)+32},32,1);return finish(0);
        }
        const auto format=static_cast<std::uint32_t>(arg(1));const auto kind=format&0x700U;
        if(kind!=0x100U && kind!=0x200U && kind!=0x400U)return finish(pdh::invalid_argument);
        const auto& name=*counter->second.path.counter;
        const auto names=pdh::instances(counter->second.path.object,count);
        std::vector<std::size_t> selected;
        for(std::size_t i=0;i<names.size();++i)
            if(counter->second.path.object==pdh::Object::Memory || counter->second.path.wildcard || pdh::fold(names[i])==pdh::fold(counter->second.path.instance))selected.push_back(i);
        // 依格式寫入 PDH_FMT_COUNTERVALUE：CStatus、對齊空間與 8 bytes union。
        const auto encode=[&](std::size_t index,std::array<std::uint8_t,16>& bytes){
            bytes.fill(0);std::uint32_t status=pdh::invalid_cstatus;std::optional<double> number;
            if(counter->second.current)number=pdh::value(counter->second,index,count,status);
            if(!number){for(unsigned b=0;b<4;++b)bytes[b]=static_cast<std::uint8_t>(status>>(b*8));return false;}
            auto result=*number;if(name.percent && !(format&0x8000U))result=std::clamp(result,0.0,100.0);
            if(format&0x2000U)result*=1000;
            std::uint64_t raw=0;
            if(kind==0x200U)raw=std::bit_cast<std::uint64_t>(result);
            else if(kind==0x400U)raw=static_cast<std::uint64_t>(static_cast<std::int64_t>(std::llround(std::clamp(result,-9.2e18,9.2e18))));
            else raw=static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(std::clamp(result,-2147483648.0,2147483647.0))));
            for(unsigned b=0;b<8;++b)bytes[8+b]=static_cast<std::uint8_t>(raw>>(b*8));return true;
        };
        if(symbol=="PdhGetFormattedCounterValue"){
            if(!arg(3))return finish(pdh::invalid_argument);memory.check(GuestAddress{arg(3)},16,Access::Write);
            if(arg(2)){memory.check(GuestAddress{arg(2)},4,Access::Write);memory.store(GuestAddress{arg(2)},32,name.type);}
            std::array<std::uint8_t,16> bytes{};
            if(selected.empty()){bytes.fill(0);for(unsigned b=0;b<4;++b)bytes[b]=static_cast<std::uint8_t>(pdh::no_instance>>(b*8));memory.write(GuestAddress{arg(3)},bytes);return finish(pdh::invalid_data);}
            const auto valid=encode(selected.front(),bytes);memory.write(GuestAddress{arg(3)},bytes);return finish(valid?0:pdh::invalid_data);
        }
        // PdhGetFormattedCounterArray：項目陣列後接實例名稱字串。
        if(!arg(2) || !arg(3))return finish(pdh::invalid_argument);
        memory.check(GuestAddress{arg(2)},4,Access::Write);memory.check(GuestAddress{arg(3)},4,Access::Write);
        if(!counter->second.current)return finish(pdh::invalid_data);
        const std::size_t item=24;std::size_t required=selected.size()*item;
        std::vector<std::vector<std::uint8_t>> encoded;
        for(const auto index:selected){auto bytes=wide?utf16_bytes(names[index]):[&]{auto value=utf16_to_utf8(names[index]);std::vector<std::uint8_t> out(value.begin(),value.end());out.push_back(0);return out;}();
            required+=bytes.size();encoded.push_back(std::move(bytes));}
        required=(required+7)&~std::size_t{7};
        const auto capacity=memory.load(GuestAddress{arg(2)},32);
        memory.store(GuestAddress{arg(3)},32,selected.size());
        if(!arg(4) || capacity<required){memory.store(GuestAddress{arg(2)},32,required);return finish(pdh::more_data);}
        memory.check(GuestAddress{arg(4)},required,Access::Write);
        auto strings=arg(4)+selected.size()*item;bool any=false;
        for(std::size_t i=0;i<selected.size();++i){
            std::array<std::uint8_t,16> bytes{};any=encode(selected[i],bytes) || any;
            const auto at=arg(4)+i*item;memory.store(GuestAddress{at},bits,strings);if(x86_)memory.store(GuestAddress{at+4},32,0);
            memory.write(GuestAddress{at+8},bytes);memory.write(GuestAddress{strings},encoded[i]);strings+=encoded[i].size();
        }
        memory.store(GuestAddress{arg(2)},32,required);
        return finish(any || selected.empty()?0:pdh::invalid_data);
    }catch(const MemoryFault&){return finish(pdh::invalid_argument);}catch(const std::bad_alloc&){return finish(pdh::memory_failure);}
}
}
