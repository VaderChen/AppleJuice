#pragma once
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <string_view>

namespace x64arm {
// 共用客體執行緒的階層事件，輸出可供 Trace Event 檢視器讀取的實際時間軸。
// 沒有啟用記錄器時不宣告正在 profiling；不模擬 PIX 的 frame replay。
class D3D9Perf {
    struct Thread {std::uint32_t depth{};std::string region;};
    std::map<std::uint64_t,Thread> threads_;
    std::ofstream output_;
    bool prohibited_{},first_{true};
    const std::chrono::steady_clock::time_point epoch_{std::chrono::steady_clock::now()};
    static std::string quote(std::string_view text){
        std::string out{"\""};constexpr char hex[]="0123456789abcdef";
        for(char c:text){const auto b=static_cast<unsigned char>(c);if(c=='"' || c=='\\'){out+='\\';out+=c;}else if(b<32){out+="\\u00";out+=hex[b>>4];out+=hex[b&15];}else out+=c;}
        out+='"';return out;
    }
    bool emit(std::uint64_t thread,char phase,std::string_view name,std::uint32_t color){
        const auto stamp=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-epoch_).count();
        if(!first_)output_<<",\n";first_=false;
        output_<<"{\"name\":"<<quote(name)<<",\"cat\":\"D3DPERF\",\"ph\":\""<<phase<<"\",\"pid\":1,\"tid\":"<<thread<<",\"ts\":"<<stamp;
        if(phase=='i')output_<<",\"s\":\"t\"";
        output_<<",\"args\":{\"color\":"<<color<<",\"region\":"<<quote(threads_[thread].region)<<"}}";
        output_.flush();return output_.good();
    }
public:
    D3D9Perf(){if(const auto* path=std::getenv("X64ARM_D3DPERF_TRACE");path && *path){output_.open(path);if(output_)output_<<"[\n";}}
    ~D3D9Perf(){if(output_.is_open()){close_events();output_<<"\n]\n";}}
    bool active()const{return output_.is_open() && output_.good() && !prohibited_;}
    void close_events(){for(auto& [id,t]:threads_)while(t.depth){--t.depth;if(output_.good())emit(id,'E',"",0);}}
    void options(std::uint32_t value){if(value&1)close_events();prohibited_=(value&1)!=0;}
    std::uint32_t begin(std::uint64_t thread,std::uint32_t color,std::string_view name){
        if(!active())return UINT32_MAX;auto& t=threads_[thread];if(t.depth>=4096)return UINT32_MAX;
        if(!emit(thread,'B',name,color))return UINT32_MAX;return t.depth++;
    }
    std::uint32_t end(std::uint64_t thread){
        if(!active())return UINT32_MAX;const auto it=threads_.find(thread);if(it==threads_.end() || !it->second.depth)return UINT32_MAX;
        --it->second.depth;return emit(thread,'E',"",0)?it->second.depth:UINT32_MAX;
    }
    void marker(std::uint64_t thread,std::uint32_t color,std::string_view name,bool region){
        if(!active())return;if(region)threads_[thread].region=name;(void)emit(thread,'i',name,color);
    }
};
}
