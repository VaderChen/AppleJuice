#include "x64arm/resource_usage.hpp"
#include "x64arm/gpu_memory.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <string>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
#include <sys/resource.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#include <mach/mach_time.h>
#endif

namespace x64arm {
namespace {
struct Counters {
    std::atomic<bool> enabled{}, gpu_unavailable{};
    std::atomic<std::uint64_t> received{}, sent{};
    std::mutex mutex;
    std::vector<std::pair<double,double>> gpu_intervals;
    std::mutex diagnostics_mutex;
    std::map<std::string,std::int64_t> diagnostic_counts;
    std::map<std::string,std::string> diagnostic_texts;
};
// 完成回呼可能比報告器活得更久；程序共用計數器不依賴報告器的物件生命週期。
Counters& counters() { static auto* value = new Counters; return *value; }
double host_seconds() {
#ifdef __APPLE__
    static const double scale = [] { mach_timebase_info_data_t info{}; mach_timebase_info(&info); return double(info.numer)/double(info.denom)/1e9; }();
    return double(mach_absolute_time())*scale;
#else
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}
struct ProcessSample { bool cpu_available{},memory_available{}; double cpu_seconds{}; std::uint64_t memory_bytes{}; };
ProcessSample process_sample() {
    ProcessSample sample; rusage usage{};
    if (!getrusage(RUSAGE_SELF,&usage)) {
        sample.cpu_available=true;
        sample.cpu_seconds=double(usage.ru_utime.tv_sec+usage.ru_stime.tv_sec)+double(usage.ru_utime.tv_usec+usage.ru_stime.tv_usec)/1e6;
    }
#ifdef __APPLE__
    rusage_info_v2 info{};
    if (!proc_pid_rusage(getpid(),RUSAGE_INFO_V2,reinterpret_cast<rusage_info_t*>(&info))) {
        sample.memory_available=true; sample.memory_bytes=info.ri_phys_footprint;
    }
#elif defined(__linux__)
    std::uint64_t virtual_pages{},resident_pages{}; std::ifstream statm("/proc/self/statm");
    if (statm>>virtual_pages>>resident_pages) { sample.memory_available=true; sample.memory_bytes=resident_pages*static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE)); }
#endif
    return sample;
}
}

bool resource_usage_enabled() noexcept { return counters().enabled.load(std::memory_order_relaxed); }
void record_network_transfer(std::uint64_t received,std::uint64_t sent) noexcept {
    auto& c=counters(); if (!c.enabled.load(std::memory_order_relaxed)) return;
    c.received.fetch_add(received,std::memory_order_relaxed); c.sent.fetch_add(sent,std::memory_order_relaxed);
}
void mark_gpu_timing_unavailable() noexcept { counters().gpu_unavailable.store(true,std::memory_order_relaxed); }
namespace { std::atomic<bool> gpu_statistics{}; std::atomic<std::uint64_t> gpu_running{}; }
void enable_gpu_statistics() noexcept { gpu_statistics.store(true,std::memory_order_relaxed); }
bool gpu_statistics_enabled() noexcept { return gpu_statistics.load(std::memory_order_relaxed); }
std::uint64_t gpu_running_time() noexcept { return gpu_running.load(std::memory_order_relaxed); }
void record_gpu_interval(double start,double end) noexcept {
    if (!std::isfinite(start) || !std::isfinite(end) || start<=0 || end<=start) return;
    if (gpu_statistics_enabled()) gpu_running.fetch_add(static_cast<std::uint64_t>((end-start)*1e7),std::memory_order_relaxed);
    if (!resource_usage_enabled()) return;
    try { auto& c=counters(); std::lock_guard lock(c.mutex); c.gpu_intervals.emplace_back(start,end); }
    catch (...) { mark_gpu_timing_unavailable(); }
}
namespace {
std::vector<std::pair<double,double>> take_gpu_intervals(double start,double end) {
    auto& c=counters(); std::lock_guard lock(c.mutex);
    std::sort(c.gpu_intervals.begin(),c.gpu_intervals.end());
    std::vector<std::pair<double,double>> merged;
    for (const auto& interval:c.gpu_intervals) {
        const auto first=std::max(start,interval.first),last=std::min(end,interval.second);
        if(last<=first)continue;
        if(!merged.empty() && first<=merged.back().second)merged.back().second=std::max(last,merged.back().second);
        else merged.emplace_back(first,last);
    }
    std::erase_if(c.gpu_intervals,[&](const auto& interval) { return interval.second<=end; });
    return merged;
}
}
double take_gpu_busy_seconds(double start,double end) {
    double busy=0;for(const auto& [first,last]:take_gpu_intervals(start,end))busy+=last-first;
    return busy;
}

struct ResourceUsageReporter::Impl {
    std::filesystem::path path;
    std::mutex mutex; std::condition_variable changed; bool stopping{};
    std::thread worker;
    std::uint64_t sequence{},last_received{},last_sent{};
    double last_time{}; ProcessSample last_process;
    explicit Impl(std::filesystem::path destination):path(std::move(destination)) {
        if (counters().enabled.exchange(true)) throw std::logic_error("資源用量報告器已啟用");
        try {
            publish(true);
            worker=std::thread([this] {
                auto next=std::chrono::steady_clock::now()+std::chrono::seconds(3);
                std::unique_lock lock(mutex);
                while (!changed.wait_until(lock,next,[this] { return stopping; })) {
                    lock.unlock();
                    // 監測檔案失敗不影響客體執行；下一次取樣仍會重新嘗試。
                    try { publish(false); } catch (...) {}
                    lock.lock(); next+=std::chrono::seconds(3);
                    if (next<std::chrono::steady_clock::now()) next=std::chrono::steady_clock::now()+std::chrono::seconds(3);
                }
            });
        } catch (...) { counters().enabled.store(false); throw; }
    }
    ~Impl() {
        { std::lock_guard lock(mutex); stopping=true; } changed.notify_one();
        if (worker.joinable()) worker.join();
        counters().enabled.store(false,std::memory_order_relaxed);
    }
    void publish(bool initial) {
        const auto now=host_seconds(); const auto process=process_sample(); const auto& c=counters();
        const auto received=c.received.load(std::memory_order_relaxed),sent=c.sent.load(std::memory_order_relaxed);
        const auto gpu=gpu_memory_stats(); const auto elapsed=initial?0:now-last_time;
        const auto intervals=initial?std::vector<std::pair<double,double>>{}:take_gpu_intervals(last_time,now);
        double gpu_busy=0;for(const auto& [first,last]:intervals)gpu_busy+=last-first;
        const auto timestamp=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        std::ostringstream json; json<<std::setprecision(12)<<"{\"version\":1,\"pid\":"<<getpid()<<",\"sequence\":"<<++sequence<<",\"sampledAt\":"<<timestamp<<",\"intervalSeconds\":"<<elapsed;
        json<<",\"cpuPercent\":";
        if (elapsed>0 && process.cpu_available && last_process.cpu_available) json<<std::max(0.0,process.cpu_seconds-last_process.cpu_seconds)/elapsed*100; else json<<"null";
        json<<",\"gpuPercent\":";
        if (elapsed>0 && !c.gpu_unavailable.load(std::memory_order_relaxed)) json<<std::clamp(gpu_busy/elapsed*100,0.0,100.0); else json<<"null";
        json<<",\"gpuTimingAvailable\":"<<(c.gpu_unavailable.load(std::memory_order_relaxed)?"false":"true")<<",\"memoryBytes\":";
        if (process.memory_available) json<<process.memory_bytes; else json<<"null";
        json<<",\"gpuMemoryBytes\":"<<gpu.used<<",\"gpuMemoryLimitBytes\":"<<gpu.limit<<",\"receivedBytes\":"<<received<<",\"sentBytes\":"<<sent<<",\"receiveBytesPerSecond\":";
        if (elapsed>0) json<<double(received-last_received)/elapsed; else json<<"null";
        json<<",\"sendBytesPerSecond\":";
        if (elapsed>0) json<<double(sent-last_sent)/elapsed; else json<<"null";
        // mach_absolute_time 為同機共用時基；Launcher 取共同觀測區間的聯集，避免多程序 GPU 重疊重算。
        json<<",\"hostStart\":"<<(initial?now:last_time)<<",\"hostEnd\":"<<now<<",\"gpuIntervals\":[";
        bool comma=false;for(const auto& [first,last]:intervals){if(comma)json<<',';comma=true;json<<'['<<first<<','<<last<<']';}
        json<<']';
        {   // 常駐診斷：計數為累計值，文字為最後一次設定的值。
            auto& counters_ref=counters();std::lock_guard lock(counters_ref.diagnostics_mutex);
            if(!counters_ref.diagnostic_counts.empty() || !counters_ref.diagnostic_texts.empty()){
                const auto quote=[&](std::string_view text){json<<'"';for(const char raw:text){const auto c=static_cast<unsigned char>(raw);
                    if(c=='"' || c=='\\')json<<'\\'<<raw;else if(c<32)json<<' ';else json<<raw;}json<<'"';};
                json<<",\"diagnostics\":{";bool first=true;
                for(const auto& [key,value]:counters_ref.diagnostic_counts){if(!first)json<<',';first=false;quote(key);json<<':'<<value;}
                for(const auto& [key,value]:counters_ref.diagnostic_texts){if(!first)json<<',';first=false;quote(key);json<<':';quote(value);}
                json<<'}';
            }
        }
        json<<"}\n";
        last_time=now; last_process=process; last_received=received; last_sent=sent;
        auto temporary=path; temporary+=".tmp";
        { std::ofstream file(temporary,std::ios::out|std::ios::trunc); file<<json.str(); file.close(); if (!file) throw std::runtime_error("無法寫入資源用量快照"); }
        std::filesystem::rename(temporary,path);
    }
};
void record_diagnostic(const char* key,std::int64_t delta) noexcept {
    auto& c=counters();if(!c.enabled.load(std::memory_order_relaxed))return;
    try{std::lock_guard lock(c.diagnostics_mutex);c.diagnostic_counts[key]+=delta;}catch(...){}
}
void set_diagnostic(const char* key,std::string_view text) noexcept {
    auto& c=counters();if(!c.enabled.load(std::memory_order_relaxed))return;
    try{std::lock_guard lock(c.diagnostics_mutex);c.diagnostic_texts[key]=std::string(text.substr(0,240));}catch(...){}
}
ResourceUsageReporter::ResourceUsageReporter(const std::filesystem::path& path) {
    auto destination=path;
    if(!destination.empty()){
        const auto root=std::filesystem::absolute(destination).string();
        std::filesystem::create_directories(root+".processes");
        std::filesystem::create_directories(root+".cache");
        ::setenv("X64ARM_USAGE_ROOT",root.c_str(),1);
        ::setenv("X64ARM_AOT_CACHE_STATS_DIRECTORY",(root+".cache").c_str(),1);
        root_=true;
    }else if(const auto* root=std::getenv("X64ARM_USAGE_ROOT");root && *root){
        destination=std::filesystem::path(std::string(root)+".processes")/(std::to_string(::getpid())+".json");
    }
    if(!destination.empty())impl_=std::make_unique<Impl>(destination);
}
ResourceUsageReporter::~ResourceUsageReporter(){
    impl_.reset();
    if(root_){::unsetenv("X64ARM_USAGE_ROOT");::unsetenv("X64ARM_AOT_CACHE_STATS_DIRECTORY");}
}
}
