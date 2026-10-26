#pragma once
#include "x64arm/readonly_files.hpp"
#include <vector>

namespace x64arm::file_stat {
// Win32 ByName、NT ByName 與 NT handle 共用固定布局；最大 104 bytes，不依呼叫端 Length 配置。
inline unsigned size(std::uint32_t kind) {
    switch(kind){case 68:return 72;case 70:return 96;case 71:return 4;case 77:return 104;default:return 0;}
}
inline std::uint32_t native_class(std::uint32_t kind) {
    switch(kind){case 0:return 68;case 1:return 70;case 2:return 71;case 3:return 77;default:return 0;}
}
inline std::vector<std::uint8_t> encode(std::uint32_t kind,const FileMetadata& info,std::uint32_t effective) {
    std::vector<std::uint8_t> bytes(size(kind));
    const auto put=[&](unsigned offset,unsigned width,std::uint64_t value){for(unsigned n=0;n<width;++n)bytes.at(offset+n)=static_cast<std::uint8_t>(value>>(n*8));};
    // 目前 Windows 磁碟名稱空間採不區分大小寫，且未提供 WSL EA 持久化；不投影宿主 UID／GID／mode。
    if(kind==71)return bytes;
    put(0,8,info.id);put(8,8,info.created);put(16,8,info.accessed);put(24,8,info.modified);put(32,8,info.changed);
    put(40,8,info.allocation_size);put(48,8,info.size);put(56,4,info.attributes);put(60,4,info.reparse_tag);put(64,4,info.links);
    if(kind==77){
        // 固定 Windows 磁碟裝置；VolumeSerialNumber 與 128-bit ID 沿用 FileIdInfo 的共用映射。
        put(68,4,7);put(80,8,info.device);put(88,8,info.id);
    }else put(68,4,effective);
    return bytes;
}
}
