#pragma once
#include "x64arm/memory.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
namespace x64arm::detail {
// 診斷：X64ARM_TRAP_DUMP_DIR=<目錄>：把主映像目前的記憶體（含執行期才解開的程式碼）逐頁寫成 image-<基底>.bin，
// 讀不到的頁補零；用來離線反組譯加殼程式。停在未實作匯入與未處理例外結束時各寫一次（同名覆寫）。只讀取。
inline void dump_main_image(const GuestMemory& memory,std::uint64_t base,std::uint64_t size) noexcept {
    const auto* directory=std::getenv("X64ARM_TRAP_DUMP_DIR");
    if(!directory || !*directory || !base || !size)return;
    try{
        char file[64];std::snprintf(file,sizeof file,"image-%llx.bin",static_cast<unsigned long long>(base));
        auto* out=std::fopen((std::string(directory)+"/"+file).c_str(),"wb");if(!out)return;
        std::vector<std::uint8_t> page(GuestMemory::guest_page_size);
        for(std::uint64_t at=0;at<size;at+=page.size()){
            const auto bytes=std::min<std::uint64_t>(page.size(),size-at);
            try{memory.read(GuestAddress{base+at},std::span(page.data(),static_cast<std::size_t>(bytes)));}
            catch(...){std::fill(page.begin(),page.end(),0);}
            std::fwrite(page.data(),1,static_cast<std::size_t>(bytes),out);
        }
        std::fclose(out);
    }catch(...){}
}
}
