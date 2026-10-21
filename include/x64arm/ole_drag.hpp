#pragma once
#include <array>
#include <bit>
#include <cstdint>

namespace x64arm::ole_drag {
inline constexpr std::uint32_t drop=0x00040100U,cancelled=0x00040101U,default_cursors=0x00040102U;
inline constexpr std::uint32_t copy=1,move=2,link=4,scroll=0x80000000U,effects=copy|move|link;

// POINTL 在 Win32 仍是兩個 LONG；交由共用 GuestCallback 的 wide_arguments 傳遞。
constexpr std::uint64_t point_value(std::array<std::int32_t,2> point){
    return std::bit_cast<std::uint32_t>(point[0])|(std::uint64_t{std::bit_cast<std::uint32_t>(point[1])}<<32);
}
constexpr std::uint32_t key_state(const std::array<std::uint8_t,256>& keys){
    std::uint32_t result=0;
    constexpr std::array<unsigned,6> virtual_keys{1,2,16,17,4,18};
    for(unsigned i=0;i<virtual_keys.size();++i)if(keys[virtual_keys[i]]&0x80U)result|=1U<<i;
    return result;
}
struct Target {
    std::uint64_t window{},object{},thread{},generation{};
    explicit operator bool() const {return window && object && thread;}
    bool operator==(const Target&) const = default;
};
struct Input {
    std::array<std::int32_t,2> point{};
    std::uint32_t keys{};
    bool escape{};
    Target target{};
    bool operator==(const Input&) const = default;
};
enum class Action { Wait, QueryContinue, AcquireTarget, Enter, Over, Leave, ReleaseTarget, Feedback, DefaultFeedback, Drop, Finish };
struct Request {
    Action action{Action::Wait};
    Target target{};
    Input input{};
    std::uint32_t effect{};
    bool operator==(const Request&) const = default;
};
struct Reply {
    // Acquire／Release 是 ULONG；status 不用作 HRESULT 判定。
    std::uint32_t status{},effect{};
    // 傳輸失敗表示 callback 未開始；已開始的客體例外由 callback unwind 清理。
    std::uint32_t transport_error{};
    bool operator==(const Reply&) const = default;
};
struct Result {
    std::uint32_t status{cancelled},effect{};
    bool operator==(const Result&) const = default;
};

// 狀態機只提出下一項工作；宿主／客體回呼、STA 排程及 HWND 命中測試由 driver 執行。
// 同時最多一個未完成 request，避免 COM 回呼重入時重新開始同一項拖放工作。
class Session {
public:
    explicit Session(std::uint32_t allowed=effects):allowed_(allowed&(effects|scroll)){}
    const Request& request() const {return request_;}
    const Result& result() const {return result_;}
    bool begin(const Input&);
    bool complete(const Reply&);
    void cancel(std::uint32_t status=cancelled);
    void invalidate(const Target&);
    bool valid() const;
    bool operator==(const Session&) const = default;
private:
    enum class Intent { Continue, Drop, Finish } intent_{Intent::Continue};
    enum class Cleanup { Switch, Reject, Finish } cleanup_{Cleanup::Finish};
    enum class FeedbackNext { Wait, Drop } feedback_next_{FeedbackNext::Wait};
    std::uint32_t allowed_{},negotiated_{};
    Input input_{};
    Target held_{};
    Request request_{};
    Result result_{};
    bool accepted_{},held_reference_{},cancel_pending_{},invalidated_{},drop_started_{};
    void issue(Action,std::uint32_t effect=0);
    void attach();
    void select();
    void close(Cleanup);
    void after_release();
    void after_feedback();
    void finish_drop();
    void finish();
    void apply_cancellation();
};
}
