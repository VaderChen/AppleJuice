#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <map>
#include <string>
namespace x64arm {
struct GpuMemoryStats {std::uint64_t limit{},used{},peak{},allocations{};};
struct GpuMemoryBudget {std::uint64_t budget{},usage{},available{},reservation{};};
class GpuMemoryReservation {
public:
    ~GpuMemoryReservation();
    GpuMemoryReservation(const GpuMemoryReservation&)=delete;
    GpuMemoryReservation& operator=(const GpuMemoryReservation&)=delete;
    std::uint64_t address()const{return address_;}
    std::size_t size()const{return size_;}
private:
    friend std::shared_ptr<GpuMemoryReservation> reserve_gpu_memory(std::size_t);
    GpuMemoryReservation();
    std::uint64_t address_{};std::size_t size_{};
    std::map<std::uint64_t,std::size_t>::node_type release_node_;
};
// 程序的虛擬 GPU 位址空間。按需保留、釋放後合併，可供不同圖形 API 共用配額。
std::size_t gpu_memory_limit_for_system(std::uint64_t physical_bytes);
std::size_t host_gpu_memory_limit();
void configure_gpu_memory(std::size_t bytes);
std::shared_ptr<GpuMemoryReservation> reserve_gpu_memory(std::size_t bytes);
GpuMemoryStats gpu_memory_stats();
// UMA 的 local segment 與所有圖形 API 共用配額；reservation 是工作集提示，不配置資源。
GpuMemoryBudget gpu_memory_budget(bool local);
bool set_gpu_memory_reservation(bool local,std::uint64_t bytes);
std::string gpu_memory_diagnostics();
}
