#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace x64arm {
// 診斷不能隨執行時間無限成長。保留首次出現的內容與順序，重複事件只累計次數；
// 超出容量仍計數，輸出明列省略數，不改變原作業的成功／失敗結果。
// 呼叫端沿用自己的狀態鎖；此物件不額外建立另一份背景紀錄。
class DiagnosticEvents {
public:
    struct Entry {std::string text;std::uint64_t count{1};bool operator==(const Entry&)const=default;};
    static constexpr std::size_t max_entries=4096,max_bytes=2*1024*1024;
    bool push_back(std::string text,std::uint64_t count=1) {
        if(!count)return false;
        const auto key=std::hash<std::string_view>{}(text);
        const auto [first,last]=index_.equal_range(key);
        for(auto it=first;it!=last;++it)if(auto& entry=events_[it->second];entry.text==text){add(entry.count,count);return false;}
        if(events_.size()>=max_entries || text.size()>max_bytes-bytes_){add(omitted_,count);return false;}
        const auto index=events_.size();bytes_+=text.size();events_.push_back({std::move(text),count});index_.emplace(key,index);return true;
    }
    void merge(const DiagnosticEvents& other) {for(const auto& e:other.events_)push_back(e.text,e.count);add(omitted_,other.omitted_);}
    void clear() {events_.clear();index_.clear();bytes_=0;omitted_=0;}
    std::size_t size()const noexcept{return events_.size();}
    std::size_t bytes()const noexcept{return bytes_;}
    std::uint64_t omitted()const noexcept{return omitted_;}
    bool operator==(const DiagnosticEvents& other)const{return events_==other.events_ && omitted_==other.omitted_;}
    const auto& entries()const noexcept{return events_;}
    std::string render()const {
        std::string result;
        for(const auto& e:events_){result+=e.text;result+='\n';if(e.count>1)result+="diagnostic_repeat="+std::to_string(e.count)+"\n";}
        if(omitted_)result+="diagnostic_omitted="+std::to_string(omitted_)+"\n";
        return result;
    }
private:
    static void add(std::uint64_t& target,std::uint64_t value)noexcept{target+=std::min(value,std::numeric_limits<std::uint64_t>::max()-target);}
    std::vector<Entry> events_;
    std::unordered_multimap<std::size_t,std::size_t> index_;
    std::size_t bytes_{};
    std::uint64_t omitted_{};
};
}
