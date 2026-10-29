#pragma once
#include "x64arm/windows_services.hpp"
#include "x64arm/process_io.hpp"
#include <thread>

namespace x64arm {
// 僅由共用檔案完成流程持有；取消或移除要求時先停止 producer。
// 宿主工作執行緒不存取 GuestMemory，也不直接修改客體 I/O 統計。
struct WindowsServices::AsyncFileCopy {
    std::atomic<bool> done{},cancel{};
    FileIoResult result;
    process_io::Counters accounting;
    std::uint64_t completion_port{},completion_key{};
    std::uint8_t completion_modes{};
    std::thread worker;
    ~AsyncFileCopy(){cancel.store(true,std::memory_order_relaxed);if(worker.joinable())worker.join();}
};
}
