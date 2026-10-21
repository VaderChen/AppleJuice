#include "x64arm/native_v8.hpp"
#include <spawn.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <cstdlib>
#include <sstream>
extern char** environ;
namespace x64arm {
NativeV8::NativeV8(){
    const auto* path=std::getenv("X64ARM_NATIVE_V8_HOST");
    if(!path || !*path)throw std::runtime_error("V8 原型需設定 X64ARM_NATIVE_V8_HOST 為 ARM64 模組執行檔");
    int to_child[2]{-1,-1},from_child[2]{-1,-1};
    const auto close_pipes=[&]{for(auto fd:to_child)if(fd>=0)::close(fd);for(auto fd:from_child)if(fd>=0)::close(fd);};
    if(::pipe(to_child) || ::pipe(from_child)){close_pipes();throw std::runtime_error("無法建立 V8 模組通道");}
    for(auto fd:to_child)::fcntl(fd,F_SETFD,FD_CLOEXEC);
    for(auto fd:from_child)::fcntl(fd,F_SETFD,FD_CLOEXEC);
#ifdef F_SETNOSIGPIPE
    ::fcntl(to_child[1],F_SETNOSIGPIPE,1);
#endif
    posix_spawn_file_actions_t actions;
    if(posix_spawn_file_actions_init(&actions)){close_pipes();throw std::runtime_error("無法初始化 V8 模組程序");}
    int error=posix_spawn_file_actions_adddup2(&actions,to_child[0],STDIN_FILENO);
    if(!error)error=posix_spawn_file_actions_adddup2(&actions,from_child[1],STDOUT_FILENO);
    for(auto fd:to_child)if(!error)error=posix_spawn_file_actions_addclose(&actions,fd);
    for(auto fd:from_child)if(!error)error=posix_spawn_file_actions_addclose(&actions,fd);
    pid_t child{};char* args[]{const_cast<char*>(path),nullptr};
    if(!error)error=::posix_spawn(&child,path,&actions,nullptr,args,environ);
    posix_spawn_file_actions_destroy(&actions);
    if(error){close_pipes();throw std::runtime_error("無法啟動 ARM64 V8 模組："+std::to_string(error));}
    ::close(to_child[0]);::close(from_child[1]);input_=to_child[1];output_=from_child[0];pid_=child;
}
NativeV8::~NativeV8(){
    if(input_>=0)::close(input_);if(output_>=0)::close(output_);
    // 客體非正常退出也回收整個後端，不讓 callback 等待或 JS 無窮迴圈留下孤兒程序。
    if(pid_>0){::kill(pid_,SIGTERM);while(::waitpid(pid_,nullptr,0)<0 && errno==EINTR){}}
}
void NativeV8::send(native_v8::Packet packet){native_v8::send(input_,std::move(packet));}
native_v8::Packet NativeV8::receive(){return native_v8::receive(output_);}
std::uint32_t NativeV8::sequence(){if(!sequence_)throw std::runtime_error("V8 請求序號已耗盡");return sequence_++;}
std::string NativeV8::diagnostics()const{std::ostringstream out;out<<"native_library=v8 protocol=1 backend=arm64-module calls="<<calls<<" callbacks="<<callbacks<<" live_isolates="<<owners.size()<<'\n';return out.str();}
}
