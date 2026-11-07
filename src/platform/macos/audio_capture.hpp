#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
namespace x64arm::audio {
// 診斷：X64ARM_AUDIO_CAPTURE=<目錄> 把每個輸出單元實際送往裝置的立體聲浮點樣本存成 WAV，供客觀檢查靜音、聲道、爆音與斷音。
// 即時 callback 只寫入無鎖環形緩衝；檔案由背景執行緒寫入，每秒更新檔頭，程序被結束時檔案仍有效。緩衝滿時計入遺失，不阻塞音訊。
class OutputCapture {
    std::vector<float> ring_=std::vector<float>(std::size_t{1}<<20);
    std::atomic<std::uint64_t> written_{},taken_{},dropped_{};std::atomic<bool> stop_{};
    std::FILE* file_{};std::uint64_t bytes_{};unsigned rate_;std::string path_;std::thread thread_;
    void header(){
        const auto size=static_cast<std::uint32_t>(std::min<std::uint64_t>(bytes_,0xffffffc0U));
        const auto put=[&](const void* data,std::size_t count){std::fwrite(data,1,count,file_);};
        const auto u32=[&](std::uint32_t value){put(&value,4);};const auto u16=[&](std::uint16_t value){put(&value,2);};
        std::fseek(file_,0,SEEK_SET);put("RIFF",4);u32(size+36);put("WAVEfmt ",8);u32(16);u16(3);u16(2);u32(rate_);u32(rate_*8);u16(8);u16(32);put("data",4);u32(size);
        std::fseek(file_,0,SEEK_END);std::fflush(file_);
    }
    void drain(){
        const auto end=written_.load(std::memory_order_acquire);auto at=taken_.load(std::memory_order_relaxed);
        for(;at<end;){
            const auto offset=at&(ring_.size()-1);const auto count=std::min<std::uint64_t>(end-at,ring_.size()-offset);
            std::fwrite(ring_.data()+offset,sizeof(float),count,file_);bytes_+=count*sizeof(float);at+=count;
        }
        taken_.store(at,std::memory_order_release);
    }
    void run(){
        auto next=std::chrono::steady_clock::now();
        while(!stop_.load(std::memory_order_acquire)){
            std::this_thread::sleep_for(std::chrono::milliseconds(50));drain();
            if(const auto now=std::chrono::steady_clock::now();now>=next){header();next=now+std::chrono::seconds(1);}
        }
        drain();header();
    }
public:
    // 未設定環境變數時回傳空指標，一般執行沒有任何成本。tag 區分輸出來源（PCM 混音、MIDI 合成器）。
    static std::unique_ptr<OutputCapture> open(const char* tag,unsigned rate,const void* identity){
        const auto* directory=std::getenv("X64ARM_AUDIO_CAPTURE");
        return directory && *directory?std::make_unique<OutputCapture>(directory,tag,rate,identity):nullptr;
    }
    OutputCapture(const char* directory,const char* tag,unsigned rate,const void* identity):rate_(rate){
        char name[96];std::snprintf(name,sizeof name,"/%s-%d-%p-%u.wav",tag,static_cast<int>(getpid()),identity,rate);
        path_=std::string(directory)+name;file_=std::fopen(path_.c_str(),"wb");if(!file_)return;
        header();thread_=std::thread([this]{run();});
    }
    ~OutputCapture(){
        stop_.store(true,std::memory_order_release);if(thread_.joinable())thread_.join();
        if(!file_)return;std::fclose(file_);
        std::fprintf(stderr,"[audio-capture] path=%s bytes=%llu dropped_samples=%llu\n",path_.c_str(),static_cast<unsigned long long>(bytes_),static_cast<unsigned long long>(dropped_.load(std::memory_order_relaxed)));
    }
    void push(std::span<const float> samples){
        if(!file_)return;
        const auto at=written_.load(std::memory_order_relaxed);
        if(at+samples.size()-taken_.load(std::memory_order_acquire)>ring_.size()){dropped_.fetch_add(samples.size(),std::memory_order_relaxed);return;}
        for(std::size_t i=0;i<samples.size();++i)ring_[(at+i)&(ring_.size()-1)]=samples[i];
        written_.store(at+samples.size(),std::memory_order_release);
    }
    // 非交錯的左右聲道（MIDI 合成器的輸出格式）。
    void push_planar(const float* left,const float* right,std::size_t frames){
        if(!file_)return;
        const auto at=written_.load(std::memory_order_relaxed);
        if(at+frames*2-taken_.load(std::memory_order_acquire)>ring_.size()){dropped_.fetch_add(frames*2,std::memory_order_relaxed);return;}
        for(std::size_t i=0;i<frames;++i){ring_[(at+i*2)&(ring_.size()-1)]=left[i];ring_[(at+i*2+1)&(ring_.size()-1)]=right[i];}
        written_.store(at+frames*2,std::memory_order_release);
    }
};
}
