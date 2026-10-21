#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <thread>
#if defined(__APPLE__) && defined(__aarch64__)
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <unistd.h>
#endif

namespace x64arm::cli {
// 僅由明確的診斷命令呼叫；不以 TSO 的成功值推論 AF／PF，也不改變產碼策略。
inline int apflg_probe() {
#if defined(__APPLE__) && defined(__aarch64__)
    struct Row {
        std::uint64_t cases{}, result_errors{}, nzcv_errors{}, af_errors{}, pf_errors{};
        std::uint64_t af_inverted_errors{}, pf_inverted_errors{}, observed_bits{};
    };
    struct Phase { int enable_result{-1}; std::array<Row,4> rows{}; };
    using Enable=int(*)(int);
    const auto enable=reinterpret_cast<Enable>(dlsym(RTLD_DEFAULT,"thread_set_x86_64_compat"));
    const auto collect=[](Phase& phase) {
        for(unsigned kind=0;kind<4;++kind){
            auto& row=phase.rows[kind];const bool subtract=kind>=2,wide=(kind&1)!=0;
            const auto mask=wide?UINT64_MAX:UINT64_C(0xffffffff),sign=wide?UINT64_C(1)<<63:UINT64_C(1)<<31;
            const auto check=[&](std::uint64_t lhs,std::uint64_t rhs){
                lhs&=mask;rhs&=mask;std::uint64_t result,flags;
                if(subtract){
                    if(wide)asm volatile("subs %0,%2,%3\n\tmrs %1,nzcv":"=&r"(result),"=&r"(flags):"r"(lhs),"r"(rhs):"cc");
                    else asm volatile("subs %w0,%w2,%w3\n\tmrs %1,nzcv":"=&r"(result),"=&r"(flags):"r"(lhs),"r"(rhs):"cc");
                }else{
                    if(wide)asm volatile("adds %0,%2,%3\n\tmrs %1,nzcv":"=&r"(result),"=&r"(flags):"r"(lhs),"r"(rhs):"cc");
                    else asm volatile("adds %w0,%w2,%w3\n\tmrs %1,nzcv":"=&r"(result),"=&r"(flags):"r"(lhs),"r"(rhs):"cc");
                }
                const auto expected=(subtract?lhs-rhs:lhs+rhs)&mask;
                const bool carry=subtract?lhs>=rhs:lhs>mask-rhs;
                const bool overflow=((subtract?lhs^rhs:~(lhs^rhs))&(lhs^expected)&sign)!=0;
                const auto nzcv=(std::uint64_t((expected&sign)!=0)<<31)|(std::uint64_t(expected==0)<<30)|
                    (std::uint64_t(carry)<<29)|(std::uint64_t(overflow)<<28);
                const bool af=((lhs^rhs^expected)&16)!=0,pf=(std::popcount(static_cast<std::uint8_t>(expected))&1)==0;
                const bool actual_af=(flags&(UINT64_C(1)<<27))!=0,actual_pf=(flags&(UINT64_C(1)<<26))!=0;
                ++row.cases;row.result_errors+=result!=expected;row.nzcv_errors+=(flags&UINT64_C(0xf0000000))!=nzcv;
                row.af_errors+=actual_af!=af;row.pf_errors+=actual_pf!=pf;
                row.af_inverted_errors+=actual_af==af;row.pf_inverted_errors+=actual_pf==pf;row.observed_bits|=flags;
            };
            // 完整低八位配對涵蓋 AF／PF 四種組合；另加入高位進位、符號邊界及固定亂數。
            for(unsigned a=0;a<256;++a)for(unsigned b=0;b<256;++b)check(a,b);
            const std::array<std::uint64_t,12> values{0,1,15,16,127,128,255,256,sign-1,sign,mask-1,mask};
            for(const auto a:values)for(const auto b:values)check(a,b);
            std::uint64_t seed=UINT64_C(0x9e3779b97f4a7c15);
            const auto next=[&](){seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return seed;};
            for(unsigned n=0;n<4096;++n){const auto a=next(),b=next();check(a,b);}
        }
    };
    std::array<Phase,6> phases{};
    collect(phases[0]); // CLI 已完成跨架構啟動的實際狀態。
    if(enable){phases[1].enable_result=enable(0);collect(phases[1]);
        phases[2].enable_result=enable(1);collect(phases[2]);}
    std::array<std::thread,3> workers;
    for(unsigned i=0;i<workers.size();++i)workers[i]=std::thread([&,i]{auto& phase=phases[i+3];
        phase.enable_result=enable?enable(1):-1;collect(phase);});
    for(auto& worker:workers)worker.join();
    std::array<char,4096> executable{};std::uint32_t size=executable.size();
    if(_NSGetExecutablePath(executable.data(),&size))return 1;
    // 路徑由 JSON 字串規則跳脫，避免空白或引號造成報告無法解析。
    std::cout<<"{\"status\":\"complete\",\"host_page_size\":"<<getpagesize()<<",\"pid\":"<<getpid()<<",\"executable\":\"";
    for(const char character:std::string_view(executable.data())){
        const auto c=static_cast<unsigned char>(character);
        if(c=='\\' || c=='\"')std::cout<<'\\'<<c;
        else if(c<32){static constexpr char digits[]="0123456789abcdef";std::cout<<"\\u00"<<digits[c>>4]<<digits[c&15];}
        else std::cout<<c;
    }
    std::cout<<"\",\"api_available\":"<<(enable?"true":"false")<<",\"phases\":[";
    constexpr const char* names[]{"startup","compat-0","compat-1","worker-0","worker-1","worker-2"};
    constexpr const char* operations[]{"adds32","adds64","subs32","subs64"};
    bool valid=true;
    for(unsigned p=0;p<phases.size();++p){if(p)std::cout<<',';const auto& phase=phases[p];
        std::cout<<"{\"name\":\""<<names[p]<<"\",\"enable_result\":"<<phase.enable_result<<",\"operations\":[";
        for(unsigned i=0;i<phase.rows.size();++i){if(i)std::cout<<',';const auto& r=phase.rows[i];
            valid&=!r.result_errors&&!r.nzcv_errors;
            std::cout<<"{\"name\":\""<<operations[i]<<"\",\"cases\":"<<r.cases<<",\"result_errors\":"<<r.result_errors
                <<",\"nzcv_errors\":"<<r.nzcv_errors<<",\"af_errors\":"<<r.af_errors<<",\"pf_errors\":"<<r.pf_errors
                <<",\"af_inverted_errors\":"<<r.af_inverted_errors<<",\"pf_inverted_errors\":"<<r.pf_inverted_errors
                <<",\"observed_bits\":"<<r.observed_bits<<'}';
        }
        std::cout<<"]}";
    }
    std::cout<<"],\"scope\":\"實測 ADDS／SUBS 後 NZCV[27:26]；AF／PF 不相符是能力結果，不以 TSO 或簽章推論可用。\"}\n";
    return valid?0:1;
#else
    std::cout<<"{\"status\":\"unsupported\",\"reason\":\"需要 Apple ARM64 宿主\"}\n";
    return 77;
#endif
}
}
