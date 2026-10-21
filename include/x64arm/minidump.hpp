#pragma once
#include "x64arm/windows_runtime.hpp"
#include <optional>

namespace x64arm {
using DumpBytes=std::vector<std::uint8_t>;
struct DumpMemory {std::uint64_t base{};DumpBytes bytes;};
struct DumpThread {
    std::uint32_t id{},suspend{},priority_class{},priority{},flags{0x17},exit_code{};
    std::uint64_t handle{},teb{},stack_begin{},stack_end{},stack_limit{},instruction{},start{},affinity{};
    DumpBytes context,stack;std::u16string name;bool writing{};
};
struct DumpModule {
    RuntimeImage image;std::u16string path;
    std::uint32_t flags{1},timestamp{},checksum{},header_size{};
    std::array<std::uint32_t,13> version{};
    struct Section {std::uint64_t base,size;std::uint32_t flags;};std::vector<Section> sections;
    DumpBytes cv,misc;std::uint64_t cv_pointer{},misc_pointer{};
};
struct DumpSnapshot {
    bool x86{};std::uint32_t flags{},pid{},processors{},timestamp{};
    std::vector<DumpThread> threads;std::vector<DumpModule> modules;
    std::vector<DumpMemory> memory;std::vector<VirtualMemoryInformation> information;
    std::vector<std::pair<std::uint32_t,DumpBytes>> user_streams;
    std::optional<std::pair<std::uint32_t,DumpBytes>> exception;
    DumpBytes exception_context;
};
inline constexpr std::size_t minidump_maximum_bytes=512U*1024U*1024U;
DumpBytes encode_minidump(const DumpSnapshot&);
DumpBytes minidump_context(const GuestState&,bool);
}
