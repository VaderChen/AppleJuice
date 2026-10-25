#pragma once
#include <cstddef>
#include <cstdint>
#include <list>
#include <iterator>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <string>

namespace x64arm {
struct BoundedCacheStats {
    std::size_t bytes{},entries{},byte_limit{},entry_limit{};
    std::uint64_t hits{},misses{},evictions{},rejections{};
};
// 可重建資料的容量與筆數雙限額 LRU。payload 必須包含 Value 所擁有的動態容量；
// 另計兩份鍵、Value 與索引開銷。傳回值由呼叫端擁有，淘汰不會撤銷仍在使用的資料。
// 快取無空間或配置失敗只拒絕保留，不把快取容量當成客體操作失敗。
template<class Value> class BoundedCache {
    struct Entry {std::string key;Value value;std::size_t charge;};
    using Order=std::list<Entry>;
    Order order_;
    std::map<std::string,typename Order::iterator,std::less<>> index_;
    mutable std::mutex mutex_;
    BoundedCacheStats stats_;
    void remove(typename Order::iterator at){
        stats_.bytes-=at->charge;index_.erase(at->key);order_.erase(at);
    }
public:
    BoundedCache(std::size_t bytes,std::size_t entries):stats_{0,0,bytes,entries}{}
    std::optional<Value> get(const std::string& key){
        std::lock_guard lock(mutex_);const auto at=index_.find(key);
        if(at==index_.end()){++stats_.misses;return {};}
        ++stats_.hits;order_.splice(order_.begin(),order_,at->second);return at->second->value;
    }
    bool put(const std::string& key,const Value& value,std::size_t payload=0){
        std::lock_guard lock(mutex_);
        constexpr std::size_t overhead=sizeof(Entry)+128;
        const auto limit=stats_.byte_limit;
        if(!stats_.entry_limit || limit<overhead || key.size()>(limit-overhead)/2 ||
           payload>limit-overhead-key.size()*2){++stats_.rejections;return false;}
        const auto charge=overhead+key.size()*2+payload;
        if(const auto at=index_.find(key);at!=index_.end())remove(at->second);
        while(!order_.empty() && (index_.size()>=stats_.entry_limit || charge>limit-stats_.bytes)){
            remove(std::prev(order_.end()));++stats_.evictions;
        }
        try{
            order_.push_front({key,value,charge});
            try{index_.emplace(order_.front().key,order_.begin());}
            catch(...){order_.pop_front();throw;}
            stats_.bytes+=charge;return true;
        }catch(const std::bad_alloc&){++stats_.rejections;return false;}
    }
    BoundedCacheStats stats()const{
        std::lock_guard lock(mutex_);auto result=stats_;result.entries=index_.size();return result;
    }
};
}
