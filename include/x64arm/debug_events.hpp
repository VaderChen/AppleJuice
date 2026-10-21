#pragma once
#include "x64arm/memory.hpp"
#include <deque>
#include <map>
#include <variant>
#include <vector>

namespace x64arm::debugger {
constexpr std::uint32_t handled=0x10002,not_handled=0x80010001,reply_later=0x40010001;
struct Error {std::uint32_t code;};
struct Exception {std::uint32_t code{},flags{};std::uint64_t record{},address{};std::vector<std::uint64_t> parameters;bool first_chance{true};bool operator==(const Exception&)const=default;};
struct CreateThread {std::uint64_t thread{},teb{},start{};bool operator==(const CreateThread&)const=default;};
struct CreateProcess {std::uint64_t file{},process{},thread{},base{},teb{},start{},image_name{};std::uint32_t debug_offset{},debug_size{};bool unicode{};bool operator==(const CreateProcess&)const=default;};
struct ExitThread {std::uint32_t code{};bool operator==(const ExitThread&)const=default;};
struct ExitProcess {std::uint32_t code{};bool operator==(const ExitProcess&)const=default;};
struct LoadDll {std::uint64_t file{},base{},image_name{};std::uint32_t debug_offset{},debug_size{};bool unicode{};bool operator==(const LoadDll&)const=default;};
struct UnloadDll {std::uint64_t base{};bool operator==(const UnloadDll&)const=default;};
struct DebugString {std::uint64_t address{};std::uint16_t length{};bool unicode{};bool operator==(const DebugString&)const=default;};
struct Rip {std::uint32_t error{},type{};bool operator==(const Rip&)const=default;};
using Payload=std::variant<Exception,CreateThread,CreateProcess,ExitThread,ExitProcess,LoadDll,UnloadDll,DebugString,Rip>;
struct Event {std::uint32_t pid{},tid{};Payload payload;bool operator==(const Event&)const=default;};
std::vector<std::uint8_t> encode(const Event&,ExecutionMode);
Event decode(std::span<const std::uint8_t>,ExecutionMode);
struct Envelope {std::uint64_t sequence{},birth{};Event event;bool operator==(const Envelope&)const=default;};
// 事件先 offer，驗證 caller buffer 並準備輸出後才 deliver；同一程序一次只交付一件。
class EventQueue {
public:
    void attach(std::uint32_t pid,std::uint64_t birth,std::uint64_t owner);
    void detach(std::uint32_t pid,std::uint64_t birth,std::uint64_t owner);
    std::uint64_t push(std::uint32_t pid,std::uint64_t birth,Event);
    const Envelope* offer(std::uint64_t owner)const;
    const Envelope& deliver(std::uint64_t owner,std::uint64_t sequence);
    Envelope continue_event(std::uint64_t owner,std::uint32_t pid,std::uint32_t tid,std::uint32_t status);
    bool attached(std::uint32_t pid,std::uint64_t birth)const;
    bool empty()const noexcept{return pending_.empty() && delivered_.empty();}
    bool operator==(const EventQueue&)const=default;
private:
    std::map<std::uint32_t,std::uint64_t> sessions_;
    std::map<std::uint32_t,Envelope> delivered_;
    std::deque<Envelope> pending_;
    std::uint64_t owner_{},next_{1};
    void check_owner(std::uint64_t)const;
};
}
