#include "x64arm/system_performance.hpp"
#include <algorithm>
#include <chrono>
#include <mutex>
#include <fstream>
#include <string>
#if !defined(_WIN32)
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/vm_statistics.h>
#include <sys/sysctl.h>
#endif
namespace x64arm {
MemoryResourceState system_memory_resource_state(const SystemPerformance& snapshot){
    if(snapshot.error)return {false,false,snapshot.error};
    const auto total=snapshot.pages[3],available=snapshot.pages[4];
    if(!total || available>total)return {false,false,13};
    // 虛擬系統使用實測 available physical pages 的 5%／10% 低、高區間。
    // 中間區間皆不發訊號，不把單一客體的配置上限當成全系統實體壓力。
    return {available<=total/20,available>=std::max<std::uint64_t>(1,total/10),0};
}
SystemCpuTime native_system_cpu_time(){
    SystemCpuTime result;
#if defined(__APPLE__)
    host_cpu_load_info_data_t load{};mach_msg_type_number_t count=HOST_CPU_LOAD_INFO_COUNT;
    const auto host=mach_host_self();const auto status=host_statistics(host,HOST_CPU_LOAD_INFO,reinterpret_cast<host_info_t>(&load),&count);
    (void)mach_port_deallocate(mach_task_self(),host);
    const auto frequency=::sysconf(_SC_CLK_TCK);
    if(status!=KERN_SUCCESS || count!=HOST_CPU_LOAD_INFO_COUNT || frequency<=0){result.error=50;return result;}
    const auto scale=[&](std::uint64_t ticks){return ticks*10000000/static_cast<std::uint64_t>(frequency);};
    result.idle=scale(load.cpu_ticks[CPU_STATE_IDLE]);
    result.kernel=scale(std::uint64_t{load.cpu_ticks[CPU_STATE_SYSTEM]}+load.cpu_ticks[CPU_STATE_IDLE]);
    result.user=scale(std::uint64_t{load.cpu_ticks[CPU_STATE_USER]}+load.cpu_ticks[CPU_STATE_NICE]);
#elif defined(__linux__)
    std::ifstream input("/proc/stat");std::string label;std::uint64_t user=0,nice=0,system=0,idle=0,iowait=0,irq=0,softirq=0,steal=0;
    const auto frequency=::sysconf(_SC_CLK_TCK);
    if(!(input>>label>>user>>nice>>system>>idle>>iowait>>irq>>softirq>>steal) || label!="cpu" || frequency<=0){result.error=50;return result;}
    const auto scale=[&](std::uint64_t ticks){return ticks*10000000/static_cast<std::uint64_t>(frequency);};
    result.idle=scale(idle+iowait);result.kernel=scale(system+irq+softirq+idle+iowait);result.user=scale(user+nice);
#else
    result.error=50;
#endif
    return result;
}
std::vector<SystemCpuTime> native_processor_cpu_times(){
    std::vector<SystemCpuTime> result;
#if defined(__APPLE__) || defined(__linux__)
    const auto frequency=::sysconf(_SC_CLK_TCK);if(frequency<=0)return result;
    const auto scale=[&](std::uint64_t ticks){return ticks*10000000/static_cast<std::uint64_t>(frequency);};
#endif
#if defined(__APPLE__)
    natural_t count=0;processor_info_array_t info=nullptr;mach_msg_type_number_t size=0;
    const auto host=mach_host_self();const auto status=host_processor_info(host,PROCESSOR_CPU_LOAD_INFO,&count,&info,&size);
    (void)mach_port_deallocate(mach_task_self(),host);
    if(status!=KERN_SUCCESS || !info)return result;
    if(size>=count*CPU_STATE_MAX){
        const auto* load=reinterpret_cast<const processor_cpu_load_info_data_t*>(info);
        for(natural_t i=0;i<count;++i){
            SystemCpuTime item;const auto& ticks=load[i].cpu_ticks;
            item.idle=scale(ticks[CPU_STATE_IDLE]);
            item.kernel=scale(std::uint64_t{ticks[CPU_STATE_SYSTEM]}+ticks[CPU_STATE_IDLE]);
            item.user=scale(std::uint64_t{ticks[CPU_STATE_USER]}+ticks[CPU_STATE_NICE]);
            result.push_back(item);
        }
    }
    (void)vm_deallocate(mach_task_self(),reinterpret_cast<vm_address_t>(info),static_cast<vm_size_t>(size)*sizeof(integer_t));
#elif defined(__linux__)
    std::ifstream input("/proc/stat");std::string label;
    while(input>>label){
        std::uint64_t user=0,nice=0,system=0,idle=0,iowait=0,irq=0,softirq=0,steal=0;
        if(label.size()>3 && label.starts_with("cpu") && (input>>user>>nice>>system>>idle>>iowait>>irq>>softirq>>steal))
            result.push_back({scale(idle+iowait),scale(system+irq+softirq+idle+iowait),scale(user+nice),0});
        input.ignore(4096,'\n');
    }
#endif
    return result;
}
SystemPerformance native_system_performance(){
    SystemPerformance result;
#if defined(__APPLE__)
    const auto host=mach_host_self();vm_size_t host_page=0;vm_statistics64_data_t statistics{};mach_msg_type_number_t count=HOST_VM_INFO64_COUNT;
    const auto page_status=host_page_size(host,&host_page);const auto vm_status=host_statistics64(host,HOST_VM_INFO64,reinterpret_cast<host_info64_t>(&statistics),&count);
    (void)mach_port_deallocate(mach_task_self(),host);
    // hw.memsize 固定不變、vm.swapusage 的 sysctl 約 0.2～0.3 ms 且變化緩慢：分別永久與每 2 秒快取一次，
    // 讓 GlobalMemoryStatusEx 之類每幀查詢只付 host_statistics64 的成本。
    static std::mutex cache_mutex;static std::uint64_t cached_physical=0;static struct xsw_usage cached_swap{};static std::chrono::steady_clock::time_point swap_at{};static bool swap_valid=false;
    std::uint64_t physical=0;struct xsw_usage swap{};bool sysctl_failed=false;
    {std::lock_guard lock(cache_mutex);const auto now=std::chrono::steady_clock::now();
        if(!cached_physical){std::size_t physical_size=sizeof cached_physical;if(::sysctlbyname("hw.memsize",&cached_physical,&physical_size,nullptr,0))cached_physical=0;}
        if(!swap_valid || now-swap_at>std::chrono::seconds(2)){std::size_t swap_size=sizeof cached_swap;if(::sysctlbyname("vm.swapusage",&cached_swap,&swap_size,nullptr,0))swap_valid=false;else {swap_valid=true;swap_at=now;}}
        physical=cached_physical;swap=cached_swap;sysctl_failed=!cached_physical || !swap_valid;}
    if(page_status!=KERN_SUCCESS || vm_status!=KERN_SUCCESS || !host_page || sysctl_failed){result.error=50;return result;}
    const auto pages=[&](std::uint64_t value){return value*host_page/4096;};
    // Darwin 沒有 NT commit reservation／paged-pool。以匿名、壓縮與 wired VM 的實測值
    // 映射負載；不是 Windows 開機以來的精確 commit／pool accounting。
    const auto committed=pages(std::uint64_t{statistics.internal_page_count}+statistics.total_uncompressed_pages_in_compressor+statistics.wire_count);
    const auto total=physical/4096,limit=std::max(committed,(physical+swap.xsu_total)/4096);
    // speculative 已含於 free_count，不重複相加；inactive 對應可回收 standby 類頁面。
    const auto available=std::min(total,pages(std::uint64_t{statistics.free_count}+statistics.inactive_count));
    const auto wired=pages(statistics.wire_count);
    result.pages={committed,limit,committed,total,available,pages(statistics.external_page_count),wired,0,wired,4096};
#else
    result.error=50;
#endif
    return result;
}
}
