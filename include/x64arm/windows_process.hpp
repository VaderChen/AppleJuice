#pragma once
#include <map>
#include <cstdint>
#include <filesystem>
#include <string>
#include <memory>
#include <array>
#include <optional>
#include "x64arm/process_startup_data.hpp"
namespace x64arm {
// 僅引擎之間傳遞的描述符與物件中繼資料，宿主物件在子程序完成匯入前保持存活。
struct InheritedHandle {
    std::uint64_t handle{}, auxiliary{};
    std::uint32_t type{}, access{}, flags{}, mode{}, pipe_mode{};
    int descriptor{-1};
    std::shared_ptr<void> lifetime;
};
struct ProcessImageSource {
    int descriptor{-1};
    std::uint64_t offset{},size{};
    std::uint16_t machine{},subsystem{};
    std::u16string path;
    std::shared_ptr<void> lifetime;
};
struct ProcessOptions {
    std::filesystem::path engine, filesystem_root, c_drive, install_root, registry_file;
    std::map<char,std::filesystem::path> drive_mappings{};
    unsigned processor_count{1};
    std::uint32_t startup_show{UINT32_MAX};
    std::optional<std::array<std::uint64_t,3>> startup_handles;
    bool startup_suspended{}, startup_detached{};
    std::uint64_t console_identity{};
    std::uint32_t console_group{};
    bool ignore_ctrl_c{};
    bool jit{true}, native_gui{}, host_clock{}, writable_files{}, trap_unresolved{};
    std::uint64_t max_instructions{UINT64_MAX}, memory_limit_mib{256}, gpu_memory_mib{256};
    std::string certificate_roots_file{};
    ProcessStartupData startup_data{};
};
// 引擎內部 IPC，與 POSIX 的八位元 process status 分離；不改 Windows DWORD exit code。
void set_child_result_descriptor(int descriptor);
void report_child_input_idle();
void report_child_job_status(std::uint32_t);
void report_child_startup_ready();
void report_child_startup_failure(std::uint32_t);
void report_child_result(std::uint32_t exit_code, bool succeeded);
std::uint32_t host_process_id() noexcept;
}
