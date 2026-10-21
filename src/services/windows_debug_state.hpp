#pragma once
#include "x64arm/debug_events.hpp"
#include "x64arm/debug_transport.hpp"
#include <chrono>
#include <optional>
#include <set>

namespace x64arm {
class WindowsServices;
struct DebugTargetState {
    debugger::Peer peer;
    std::uint64_t controller_thread{},delivered{},continued{};
    std::uint64_t initial_thread{};
    std::uint32_t disposition{debugger::not_handled};
    debugger::EventQueue events;
    std::set<std::uint64_t> modules;
    bool unicode{};
    std::uint64_t ansi_sequence{};
    debugger::DebugString ansi_string;
    bool disconnected{};
};
struct DebugControllerState {
    debugger::Peer peer;
    std::uint64_t owner{},process_handle{};
    std::map<std::uint32_t,std::uint64_t> thread_handles;
    std::optional<debugger::Envelope> offered,delivered;
    bool disconnected{},unicode{};
};
// 記憶體與偵錯事件共用已驗證 PID／出生身分的原生 stream。
// 只能由 owner 的協調執行緒操作，複製 WindowsServices 不取得通道所有權。
struct DebugMemoryState {
    WindowsServices* owner{};
    debugger::Transport transport;
    std::uint64_t next{1};
    std::uint64_t breakpoint_page{},breakpoint_thread{};
    std::uint64_t debug_string_page{};
    std::set<std::uint64_t> unicode_owners;
    std::map<std::pair<std::uint64_t,std::uint64_t>,debugger::Frame> replies;
    std::map<std::uint64_t,std::uint64_t> active;
    std::set<std::uint64_t> disconnected;
    std::vector<debugger::TransportEvent> pending;
    std::chrono::steady_clock::time_point next_idle_poll{};
    std::optional<DebugTargetState> target;
    std::map<std::uint32_t,DebugControllerState> controllers;
    DebugMemoryState(WindowsServices* service,const std::filesystem::path& directory):owner(service),transport(directory){}
};
}
