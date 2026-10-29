#pragma once
#include <cstdint>
#include <deque>
#include <array>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>
namespace x64arm::midi {
struct Event {std::uint64_t frame{};std::uint8_t status{},first{},second{};std::vector<std::uint8_t> sysex{};};
struct Tempo {std::uint32_t music{};std::uint64_t frame{};double bpm{120};};
struct TimeSignature {std::uint32_t music{};std::uint8_t beats{4},beat{4};std::uint16_t grids{4};};
struct Sequence {
    std::vector<Event> events;
    std::vector<Tempo> tempos{{0,0,120}};
    std::vector<TimeSignature> signatures{{0,4,4,4}};
    std::uint64_t frames{};std::uint32_t music_length{};
    std::uint64_t frame_at(std::uint32_t music) const;
    std::uint32_t music_at(std::uint64_t frame) const;
};
// Standard MIDI file format 0/1, PPQ division; timeline is 48 kHz.
Sequence parse(std::span<const std::uint8_t> bytes);
std::vector<Event> chase(const Sequence&,std::uint64_t frame);
struct Stream;
struct Playback {
    std::mutex mutex;
    std::shared_ptr<const Sequence> sequence;
    std::size_t next{};
    std::uint64_t position{},elapsed{};
    std::uint32_t repeats{};
    bool playing{},reset{true},failed{};
    std::uint64_t rendered_frames{};
    float rendered_peak{};
    std::array<float,2> rendered_channel_peak{};
    float left_gain{1},right_gain{1};
    std::shared_ptr<Stream> stream;
    struct Voice {
        std::shared_ptr<const Sequence> sequence;
        std::uint64_t start{},stop{UINT64_MAX},loop_start{},loop_end{},id{},loops{};
        double position{};
        std::size_t next{};
        std::uint32_t repeats{};
        bool playing{true},started{},finished{},aborted{},secondary{};
        std::array<std::array<bool,128>,16> notes{};
        std::vector<Event> initial_chase,loop_chase;bool chased{};
    };
    std::vector<std::shared_ptr<Voice>> voices;
    struct Message {std::uint64_t frame{};std::vector<Event> events;std::vector<std::uint8_t> sysex;double tempo{};};
    std::vector<Message> messages;
    std::array<std::array<unsigned,128>,16> note_owners{};
    // DirectMusic Port 的軟體聲部限制及 MIDI 通道優先權。
    struct DirectNote {bool active{};std::uint64_t order{};};
    std::array<std::array<DirectNote,128>,16> direct_notes{};
    std::array<std::uint32_t,16> channel_priorities{};
    std::uint32_t voice_limit{},active_notes{},lost_notes{};
    std::uint64_t note_serial{};
    std::uint64_t clock_frames{};
    double tempo_scale{1};
    double tempo_override{};
    float gain{1},gain_target{1};std::uint64_t gain_remaining{};bool active{true},mono{};
    using Send=void(*)(void*,std::uint8_t,std::uint8_t,std::uint8_t,unsigned);
    using SendSysex=void(*)(void*,std::span<const std::uint8_t>);
    // Called with mutex held; callbacks never access guest memory.
    void advance(unsigned frames,Send send,void* context,SendSysex sysex=nullptr);
};
// WinMM 串流的 48 kHz 時基；只持有宿主副本，不在音訊執行緒存取客體記憶體。
struct Stream {
    struct Event {
        std::uint32_t delta{},word{},offset{},bytes{};
        std::vector<std::uint8_t> data;
    };
    struct Buffer {
        std::uint64_t token{};
        std::vector<Event> events;
        std::size_t next{},reported{};
        bool done{};
    };
    std::deque<std::shared_ptr<Buffer>> buffers;
    std::uint32_t tempo{500000},division{480};
    std::uint8_t queued_status{};
    std::uint64_t frames{},bytes{};
    long double ticks{},event_tick{};
    bool running{},stopped{true},armed{};
    std::array<std::array<bool,128>,16> notes{};
    static bool valid_division(std::uint32_t);
    static std::vector<Event> parse(std::span<const std::uint8_t>,std::uint8_t& running_status);
    void advance(unsigned,Playback::Send,void*,Playback::SendSysex);
};
class Output {
public:
    virtual ~Output()=default;
    virtual bool short_message(std::uint8_t status,std::uint8_t first,std::uint8_t second)=0;
    virtual bool system_exclusive(std::span<const std::uint8_t> bytes)=0;
};
struct InputDevice {std::uint32_t endpoint{};std::string name;};
struct InputPacket {std::uint32_t milliseconds{};std::vector<std::uint8_t> bytes;bool overflow{};};
class Input {
public:
    virtual ~Input()=default;
    virtual void start()=0;
    virtual void stop()=0;
    virtual std::vector<InputPacket> drain()=0;
};
std::vector<InputDevice> input_devices();
std::unique_ptr<Input> open_input(std::uint32_t endpoint);
bool output_available();
std::unique_ptr<Output> open_output(std::shared_ptr<Playback>);
}
