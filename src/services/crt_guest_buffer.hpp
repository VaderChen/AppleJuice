#pragma once
#include "x64arm/windows_runtime.hpp"
#include "x64arm/memory.hpp"

namespace x64arm {
// STL 可透過 _get_stream_buffer_pointers 直接更新 FILE 欄位；與 API 共用相同緩衝。
inline void import_crt_buffer(const GuestMemory& memory,CrtFile& file,bool x86){
    auto& buffer=file.buffer;if(!buffer.capacity || !file.stream_address || buffer.direction==CrtFile::Buffer::Direction::None)return;
    const auto width=static_cast<std::uint8_t>(x86?32:64);const auto next=memory.load(GuestAddress{file.stream_address},width);
    const auto count=memory.load(GuestAddress{file.stream_address+(x86?8U:16U)},32);
    if(next<buffer.address || next-buffer.address>buffer.capacity)return; // 客體寫壞 FILE 時不推測新的宿主範圍。
    const auto offset=static_cast<std::uint32_t>(next-buffer.address);
    if(buffer.direction==CrtFile::Buffer::Direction::Write){if(count==buffer.capacity-offset)buffer.used=offset;}
    else if(offset<=buffer.used && count==buffer.used-offset)buffer.cursor=offset;
}
}
