#include "x64arm/gpu_memory.hpp"
#include "x64arm/platform.hpp"
#include <algorithm>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
namespace x64arm {
namespace {
constexpr std::size_t page_size=65536;
constexpr std::uint64_t base_address=0x100000000ULL;
struct Pool {
    std::mutex mutex;
    GpuMemoryStats stats{256*1024*1024,0,0,0};
    std::uint64_t reservation{};
    std::map<std::uint64_t,std::size_t> free{{base_address,256*1024*1024}};
};
Pool& pool(){static Pool p;return p;}
}
std::size_t gpu_memory_limit_for_system(std::uint64_t physical_bytes){
    constexpr std::uint64_t step=32*1024*1024;
    const auto maximum=std::min(physical_bytes/2,static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()));
    return static_cast<std::size_t>(maximum/step*step);
}
std::size_t host_gpu_memory_limit(){
    static const auto maximum=gpu_memory_limit_for_system(host_physical_memory());
    if(maximum<16*1024*1024)throw std::runtime_error("宿主實體記憶體不足以設定 GPU 容量");
    return maximum;
}
void configure_gpu_memory(std::size_t bytes){
    if(bytes<16*1024*1024 || bytes>host_gpu_memory_limit() || bytes%page_size)
        throw std::invalid_argument("GPU 記憶體至少為 16 MiB、不得超過宿主實體記憶體的 50%，且為 64 KiB 的倍數");
    std::map<std::uint64_t,std::size_t> replacement{{base_address,bytes}};
    auto& p=pool();std::lock_guard lock(p.mutex);
    if(p.stats.used)throw std::logic_error("GPU 記憶體使用中，無法調整容量");
    p.free.swap(replacement);p.stats={bytes,0,0,0};p.reservation=std::min<std::uint64_t>(p.reservation,bytes);
}
GpuMemoryReservation::GpuMemoryReservation(){
    // 提前準備歸還節點；釋放 GPU 資源時不再向主機配置記憶體。
    std::map<std::uint64_t,std::size_t> spare{{0,0}};
    release_node_=spare.extract(spare.begin());
}
std::shared_ptr<GpuMemoryReservation> reserve_gpu_memory(std::size_t bytes){
    if(!bytes || bytes>std::numeric_limits<std::size_t>::max()-(page_size-1))throw std::bad_alloc();
    const auto rounded=(bytes+page_size-1)&~(page_size-1);
    // 控制區塊的配置可能失敗，必須在鎖外完成且尚未啟用 reservation。
    auto result=std::shared_ptr<GpuMemoryReservation>(new GpuMemoryReservation);
    auto& p=pool();std::lock_guard lock(p.mutex);
    for(auto it=p.free.begin();it!=p.free.end();++it){
        if(it->second<rounded)continue;
        const auto address=it->first;const auto remain=it->second-rounded;
        auto node=p.free.extract(it);
        if(remain){node.key()=address+rounded;node.mapped()=remain;p.free.insert(std::move(node));}
        result->address_=address;result->size_=rounded;
        p.stats.used+=rounded;p.stats.peak=std::max(p.stats.peak,p.stats.used);++p.stats.allocations;
        return result;
    }
    throw std::bad_alloc();
}
GpuMemoryReservation::~GpuMemoryReservation(){
    if(!size_)return;
    auto& p=pool();std::lock_guard lock(p.mutex);
    auto address=address_;auto size=size_;
    auto next=p.free.lower_bound(address);
    if(next!=p.free.end() && address+size==next->first){size+=next->second;p.free.erase(next);}
    next=p.free.lower_bound(address);
    if(next!=p.free.begin()){
        auto previous=std::prev(next);
        if(previous->first+previous->second==address){address=previous->first;size+=previous->second;p.free.erase(previous);}
    }
    release_node_.key()=address;release_node_.mapped()=size;p.free.insert(std::move(release_node_));
    p.stats.used-=size_;
}
GpuMemoryStats gpu_memory_stats(){auto& p=pool();std::lock_guard lock(p.mutex);return p.stats;}
GpuMemoryBudget gpu_memory_budget(bool local){
    if(!local)return {};
    auto& p=pool();std::lock_guard lock(p.mutex);
    return {p.stats.limit,p.stats.used,p.stats.limit,p.reservation};
}
bool set_gpu_memory_reservation(bool local,std::uint64_t bytes){
    if(!local)return bytes==0;
    auto& p=pool();std::lock_guard lock(p.mutex);
    if(bytes>p.stats.limit)return false;
    p.reservation=bytes;return true;
}
std::string gpu_memory_diagnostics(){
    const auto s=gpu_memory_stats();std::ostringstream out;
    out<<"gpu_memory_limit_bytes="<<s.limit<<" gpu_memory_used_bytes="<<s.used<<" gpu_memory_peak_bytes="<<s.peak<<" gpu_memory_allocations="<<s.allocations<<'\n';
    return out.str();
}
}
