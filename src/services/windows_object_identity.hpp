#pragma once
#include <cstdint>
#include <utility>
#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace x64arm::detail {
// 重新 Open／跨程序複製的同步物件會有不同的 C++ 包裝；以實際共享檔案身分比較。
// 與一般 metadata 身分合併使用，僅讀取當次資料，不建立身分快取。
template<class Runtime>
std::pair<std::uint64_t,std::uint64_t> object_identity(const Runtime& runtime,std::uint64_t handle,std::uint64_t fallback){
#if !defined(_WIN32)
    int descriptor=-1;
    if(const auto found=runtime.wait_objects.find(handle);found!=runtime.wait_objects.end()){
        const auto& object=found->second;
        if(object.shared_event)descriptor=object.shared_event->descriptor();
        else if(object.mutex)if(const auto mutex=runtime.mutex_objects.find(object.mutex);mutex!=runtime.mutex_objects.end() && mutex->second.shared)
            descriptor=mutex->second.shared->descriptor();
    }
    struct stat info{};
    if(descriptor>=0 && !::fstat(descriptor,&info))return {static_cast<std::uint64_t>(info.st_dev),static_cast<std::uint64_t>(info.st_ino)};
#else
    (void)runtime;(void)handle;
#endif
    return {0,fallback};
}
}
