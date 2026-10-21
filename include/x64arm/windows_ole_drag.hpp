#pragma once
#include "x64arm/ole_drag.hpp"

namespace x64arm {
struct OleDragOperation {
    enum class Stage { DataRef,SourceRef,CreateCapture,Capture,Action,Marshal,Read,Dispatch,Wait,ProxyRelease,Cleanup } stage{Stage::DataRef};
    ole_drag::Session session;
    ole_drag::Input input{};
    ole_drag::Request target_request{};
    std::uint64_t thread{},data{},source{},output{},buffer{},capture{},capture_generation{},escape_presses{};
    std::uint64_t original_cursor{},default_cursor{},data_proxy{},proxy_thread{},target_rpc{};
    std::uint32_t target_result{},target_error{},quit_code{};
    unsigned cleanup{};
    bool data_ref{},source_ref{},target_ref{},target_active{},abandoned{},quit{};
    bool operator==(const OleDragOperation&) const = default;
};
}
