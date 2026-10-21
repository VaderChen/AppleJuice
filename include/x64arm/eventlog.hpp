#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
namespace x64arm {
struct EventSource {
    std::u16string source,log;
    bool operator==(const EventSource&) const = default;
};
struct EventRecord {
    std::vector<std::uint8_t> bytes;
    std::u16string channel;
    std::uint64_t parent{};
    bool operator==(const EventRecord&) const = default;
};
struct EventQuery {
    std::vector<EventRecord> records;
    std::size_t position{};
    std::uint64_t thread{};
    bool operator==(const EventQuery&) const = default;
};
struct EventRenderContext {
    std::uint32_t flags{};
    std::vector<std::string> paths;
    bool operator==(const EventRenderContext&) const = default;
};
struct LegacyEventReader {
    std::u16string channel;
    std::int64_t position{};bool positioned{};
    bool operator==(const LegacyEventReader&) const=default;
};
struct EventLogState {
    std::uint64_t next{0x69000000};
    std::map<std::uint64_t,EventSource> sources;
    std::map<std::uint64_t,LegacyEventReader> readers;
    std::map<std::uint64_t,EventQuery> queries;
    std::map<std::uint64_t,EventRecord> events;
    std::map<std::uint64_t,EventRenderContext> contexts;
    bool operator==(const EventLogState&) const = default;
};
}
