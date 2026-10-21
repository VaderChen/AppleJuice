#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace x64arm::debugger {
constexpr std::uint64_t wire_magic=UINT64_C(0x314742444e49574d);
constexpr std::size_t wire_header=32,wire_payload_limit=1024U*1024U,wire_queue_limit=3U*1024U*1024U;
struct Frame {std::uint32_t kind{},error{},flags{};std::uint64_t sequence{};std::vector<std::uint8_t> payload;bool operator==(const Frame&)const=default;};
std::vector<std::uint8_t> encode_frame(const Frame&);
struct Peer {std::uint64_t connection{},birth{};std::uint32_t pid{};bool operator==(const Peer&)const=default;};
struct TransportEvent {enum class Kind {Connected,Message,Disconnected};Kind kind{};Peer peer;Frame frame;int error{};};
// 原生通道只運送有界資料；guest 事件、權限與暫停由協調執行緒管理。
class Transport {
public:
    explicit Transport(const std::filesystem::path& directory);
    ~Transport();Transport(Transport&&)noexcept;Transport& operator=(Transport&&)noexcept;
    Transport(const Transport&)=delete;Transport& operator=(const Transport&)=delete;
    Peer connect(std::uint32_t pid,std::uint64_t birth);
    void send(const Peer&,const Frame&);
    void close(const Peer&);
    std::vector<TransportEvent> poll(unsigned milliseconds=0);
    // 只有 listener，且沒有待交付事件時，可由外部 listener 監看取代空輪詢。
    bool listener_only() const noexcept;
    std::filesystem::path path()const;
    std::uint32_t pid()const;
    std::uint64_t birth()const;
    // 監聽 socket 描述元；供排程器的描述元監看執行緒等待連線，不讀取或修改連線狀態。
    int listener_descriptor()const noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
