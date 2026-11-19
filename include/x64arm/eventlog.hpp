#pragma once
#include <cstdint>
#include <map>
#include <set>
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
    // OpenBackupEventLog：開啟時讀入的備份檔紀錄；NotifyChangeEventLog 登記的事件物件 handle（0 表示未登記）。
    bool from_backup{};std::vector<EventRecord> backup;std::uint64_t notify{};
    bool operator==(const LegacyEventReader&) const=default;
};
// EvtOpenChannelEnum 的通道名稱與目前位置。
struct EventChannelEnum {
    std::vector<std::u16string> names;
    std::size_t position{};
    bool operator==(const EventChannelEnum&) const = default;
};
// EvtOpenChannelConfig：開啟時讀入登錄設定；EvtSetChannelConfigProperty 只改這份副本，EvtSaveChannelConfig 才寫回登錄（dirty 為屬性 ID 位元）。
struct EventChannelConfig {
    std::u16string channel, display, access, file;
    std::uint64_t max_size{};
    bool retention{}, auto_backup{};
    std::uint32_t dirty{};
    bool operator==(const EventChannelConfig&) const = default;
};
// EvtOpenLog：通道或匯出檔。
struct EventLogFile {
    std::u16string channel, display, path;
    bool from_file{};
    bool operator==(const EventLogFile&) const = default;
};
// EvtCreateBookmark／EvtUpdateBookmark：通道與紀錄編號。
struct EventBookmark {
    std::u16string channel;
    std::uint32_t record{};
    bool set{};
    bool operator==(const EventBookmark&) const = default;
};
// EvtSubscribe：拉取模式（signal 事件＋EvtNext）或推送模式（callback 在執行緒池呼叫）；next 是下一筆要交付的紀錄編號。
struct EventSubscription {
    std::u16string channel, display;
    std::string query;
    std::uint32_t next{};
    std::uint64_t signal{}, callback{}, context{};
    bool queued{};
    std::uint64_t delivering{}; // 正在執行推送回呼的客體執行緒（0 表示沒有）。
    // EvtClose 在其他執行緒回呼期間等待：1＝已要求關閉（不再交付新紀錄），2＝等待完成，待服務重新進入時移除。
    std::uint8_t closing{};
    bool operator==(const EventSubscription&) const = default;
};
struct EventLogState {
    std::uint64_t next{0x69000000};
    std::map<std::uint64_t,EventSource> sources;
    std::map<std::uint64_t,LegacyEventReader> readers;
    std::map<std::uint64_t,EventQuery> queries;
    std::map<std::uint64_t,EventRecord> events;
    std::map<std::uint64_t,EventRenderContext> contexts;
    std::set<std::uint64_t> sessions;
    std::map<std::uint64_t,EventChannelEnum> channel_enums;
    std::map<std::uint64_t,EventChannelConfig> channel_configs;
    std::map<std::uint64_t,EventLogFile> log_files;
    std::map<std::uint64_t,EventBookmark> bookmarks;
    std::map<std::uint64_t,EventSubscription> subscriptions;
    bool operator==(const EventLogState&) const = default;
};
}
