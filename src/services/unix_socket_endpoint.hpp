#pragma once
#include <cerrno>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace x64arm::detail {
// 端點檔名含 process id，留在 mapping session 目錄：前一個同 id 的程序被強制結束時檔案不會刪除，PID 循環後新程序 bind 會 EADDRINUSE。
// 以同型 socket connect 探測（不送資料）：沒有程序在聽時回 ECONNREFUSED，刪掉殘留後重試；仍有存活的擁有者則維持原錯誤。
// 失敗時回 -1 並設定 errno。
inline int bind_unix_endpoint(int descriptor,const sockaddr_un& location,int type) noexcept {
    const auto* address=reinterpret_cast<const sockaddr*>(&location);
    if(!::bind(descriptor,address,sizeof(location)))return 0;
    if(errno!=EADDRINUSE)return -1;
    const int probe=::socket(AF_UNIX,type,0);
    if(probe<0){errno=EADDRINUSE;return -1;}
    const bool stale=::connect(probe,address,sizeof(location))<0 && errno==ECONNREFUSED;
    ::close(probe);
    if(!stale){errno=EADDRINUSE;return -1;}
    if(::unlink(location.sun_path))return -1;
    return ::bind(descriptor,address,sizeof(location));
}
}
