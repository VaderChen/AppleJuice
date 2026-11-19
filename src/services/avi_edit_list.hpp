#pragma once
#include "avi_container.hpp"
#include <set>

namespace x64arm::avi {
// 區段只保存原串流及其樣本範圍；複製、剪貼均不改動原始影音。
struct EditPart {std::uint64_t stream;unsigned first,count;};
struct EditList {
    std::array<std::uint8_t,204> info{};
    std::vector<EditPart> parts;
    bool decode_video{};
    unsigned refs{1};
    std::set<std::uint64_t> sources() const{
        std::set<std::uint64_t> result;for(const auto& p:parts)result.insert(p.stream);return result;
    }
    void append(EditPart part){
        if(!part.count)return;
        if(!parts.empty() && parts.back().stream==part.stream && parts.back().first+parts.back().count==part.first)
            parts.back().count+=part.count;
        else parts.push_back(part);
    }
    void append_slice(const EditList& source,unsigned offset,unsigned count){
        for(auto part:source.parts){
            if(!count)break;
            if(offset>=part.count){offset-=part.count;continue;}
            const auto take=std::min(count,part.count-offset);
            append({part.stream,part.first+offset,take});count-=take;offset=0;
        }
    }
};
}
