#pragma once
#include "x64arm/native_v8_protocol.hpp"
#include <map>
#include <memory>
namespace x64arm {
// 每個引擎程序建立一個 ARM64 模組程序；不共用其他 WindowsServices 的 heap 或執行狀態。
class NativeV8 {
public:
    NativeV8();~NativeV8();NativeV8(const NativeV8&)=delete;NativeV8& operator=(const NativeV8&)=delete;
    void send(native_v8::Packet);
    native_v8::Packet receive();
    std::uint32_t sequence();
    std::string diagnostics()const;
    struct Binding {std::uint64_t target,userdata,owner;std::uint32_t isolate;};
    struct Pending {std::uint64_t request,scratch,owner;std::uint32_t sequence,isolate,binding;};
    std::map<std::uint32_t,Binding> bindings;
    std::map<std::uint32_t,Pending> pending;
    std::map<std::uint32_t,std::uint64_t> owners;
    std::uint32_t next_binding{1};
    std::uint64_t calls{},callbacks{};
private:
    int input_{-1},output_{-1},pid_{-1};std::uint32_t sequence_{1};
};
}
