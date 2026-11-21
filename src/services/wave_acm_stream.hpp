#pragma once

#include "pcm_wave_format.hpp"
#include <memory>
#include <vector>

namespace x64arm { struct AcmState; }
namespace x64arm::wave {
// WinMM 的波形對應器沿用 ACM Stream 進行編解碼，裝置只收送相同聲道配置的 PCM。
class AcmStream {
public:
    static std::unique_ptr<AcmStream> open(GuestMemory& memory,std::uint64_t format,bool capture,const AcmState* catalog);
    ~AcmStream();
    PcmWaveFormat pcm_format()const;
    unsigned wire_alignment()const;
    unsigned packet_frames()const;
    std::uint64_t wire_bytes(std::uint64_t frames)const;
    void reset();
    std::vector<std::uint8_t> convert(std::span<const std::uint8_t> input,std::size_t limit,bool end=false);
private:
    struct Data;
    explicit AcmStream(std::unique_ptr<Data> data);
    std::unique_ptr<Data> data_;
};
}
