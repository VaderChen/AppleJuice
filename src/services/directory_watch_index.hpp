#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace x64arm::directory_detail {
// 借用同一輪詢期間不變的監看清單；不保存到下一輪，也不解參考已關閉 fd 的事件資料。
template<class Map,class Descriptor> class EventPaths {
    struct Entry {std::uintptr_t descriptor;const std::string* path;};
    const Map& paths_;
    Descriptor descriptor_;
    std::vector<Entry> sorted_;
    const std::string* linear(std::uintptr_t descriptor)const{
        for(const auto& [path,item]:paths_)if(descriptor_(item)==descriptor)return &path;
        return nullptr;
    }
    const std::string* indexed(std::uintptr_t descriptor)const{
        const auto found=std::lower_bound(sorted_.begin(),sorted_.end(),descriptor,
            [](const auto& item,auto value){return item.descriptor<value;});
        return found==sorted_.end() || found->descriptor!=descriptor?nullptr:found->path;
    }
public:
    EventPaths(const Map& paths,Descriptor descriptor):paths_(paths),descriptor_(descriptor){}
    template<class Events,class Key,class Visit> void visit(const Events& events,Key key,Visit callback){
        // 在整批入口分流，避免小型清單的每個事件都檢查索引狀態。
        if(paths_.size()<=16 || (sorted_.empty() && events.size()<=8)){
            for(const auto& event:events)callback(event,linear(key(event)));
            return;
        }
        if(sorted_.empty()){
            sorted_.reserve(paths_.size());
            for(const auto& [path,item]:paths_)sorted_.push_back({descriptor_(item),&path});
            std::sort(sorted_.begin(),sorted_.end(),[](const auto& a,const auto& b){
                return a.descriptor!=b.descriptor?a.descriptor<b.descriptor:*a.path<*b.path;
            });
        }
        for(const auto& event:events)callback(event,indexed(key(event)));
    }
};
using Identity=std::pair<std::uint64_t,std::uint64_t>;
template<class Node> Identity identity(const Node& node){return {node.info.device,node.info.id};}
template<class Node> struct SnapshotDelta {
    using Iterator=typename std::map<std::string,Node>::const_iterator;
    std::map<Identity,std::vector<Iterator>> removed,added;
};
// 兩份快照已依路徑排序；合併走訪只比較一次每個路徑，並借用變更項目。
template<class Node> SnapshotDelta<Node> snapshot_delta(const std::map<std::string,Node>& before,
                                                     const std::map<std::string,Node>& after){
    SnapshotDelta<Node> result;auto old=before.begin(),current=after.begin();
    while(old!=before.end() || current!=after.end()){
        const auto order=old==before.end()?1:current==after.end()?-1:old->first.compare(current->first);
        if(order<0){result.removed[identity(old->second)].push_back(old);++old;}
        else if(order>0){result.added[identity(current->second)].push_back(current);++current;}
        else{
            if(identity(old->second)!=identity(current->second)){
                result.removed[identity(old->second)].push_back(old);
                result.added[identity(current->second)].push_back(current);
            }
            ++old;++current;
        }
    }
    return result;
}
}
