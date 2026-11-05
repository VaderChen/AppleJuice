#pragma once
#include <CoreAudio/CoreAudio.h>
#include <string_view>
namespace x64arm::audio {
// PCM 與 MIDI 共用同一個穩定裝置 UID 查詢。
AudioDeviceID device_for(std::string_view uid);
}
