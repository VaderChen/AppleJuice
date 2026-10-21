#pragma once
#include "x64arm/named_pipe.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace x64arm {
struct SerialTransfer {
    enum class Kind { Read, Write, Event, Flush } kind{};
    std::vector<std::uint8_t> data;
    std::array<std::uint32_t,5> timeouts{};
    std::uint64_t started{},last_byte{},generation{};
    std::uint32_t position{},events{};
};
class NativeSerial {
public:
    struct Opened;
    static std::vector<std::string> devices();
    static bool is_device(const std::string&);
    static Opened open(const std::string&,std::uint32_t access);
    ~NativeSerial();
    std::uint32_t get_state(std::array<std::uint8_t,28>&);
    std::uint32_t set_state(const std::array<std::uint8_t,28>&);
    std::array<std::uint32_t,5> timeouts() const;
    std::uint32_t timeouts(const std::array<std::uint32_t,5>&);
    std::uint32_t setup(std::uint32_t,std::uint32_t);
    std::uint32_t mask() const;
    std::uint32_t mask(std::uint32_t);
    std::uint32_t modem(std::uint32_t&);
    std::uint32_t escape(unsigned);
    std::uint32_t purge(unsigned);
    std::uint32_t clear(std::uint32_t&,std::array<std::uint32_t,3>&);
    std::shared_ptr<SerialTransfer> transfer(SerialTransfer::Kind,std::vector<std::uint8_t>);
    PipeResult poll(SerialTransfer&);
private:
    struct Impl;
    explicit NativeSerial(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
struct NativeSerial::Opened {std::shared_ptr<NativeSerial> port;std::uint32_t error{};};
}
