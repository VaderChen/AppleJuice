#pragma once
#include <algorithm>
#include <cassert>
#include <bit>
#include <cstdint>
#include <poll.h>
#include <span>
#include <stdexcept>
#include <vector>

namespace x64arm::socket_detail {
struct PollGroups {
    std::vector<pollfd> queried;
    std::vector<std::size_t> indices;
    std::vector<std::uint64_t> handles;
};
// 保留第一次出現的 fd／handle 順序；每個來源項目仍有獨立回填位置。
inline PollGroups group_poll_descriptors(std::span<const pollfd> native,
                                       std::span<const std::uint64_t> handles) {
    assert(native.size()==handles.size());
    PollGroups result;
    result.indices.resize(native.size(),SIZE_MAX);
    result.queried.reserve(native.size());result.handles.reserve(native.size());
    if(native.size()<=16){
        for(std::size_t i=0;i<native.size();++i)if(native[i].fd>=0){
            const auto found=std::find_if(result.queried.begin(),result.queried.end(),
                [&](const auto& entry){return entry.fd==native[i].fd;});
            result.indices[i]=static_cast<std::size_t>(found-result.queried.begin());
            if(found==result.queried.end()){
                result.queried.push_back(native[i]);result.handles.push_back(handles[i]);
            }else found->events|=native[i].events;
        }
        return result;
    }
    // 儲存 queried 索引加一，零為空槽；連續儲存避免逐 fd 配置雜湊節點。
    std::vector<std::size_t> by_fd;
    const auto slot=[&](int fd)->std::size_t&{
        auto hash=static_cast<std::uint32_t>(fd);
        hash^=hash>>16;hash*=0x7feb352dU;hash^=hash>>15;
        auto at=static_cast<std::size_t>(hash)&(by_fd.size()-1);
        while(by_fd[at] && result.queried[by_fd[at]-1].fd!=fd)at=(at+1)&(by_fd.size()-1);
        return by_fd[at];
    };
    for(std::size_t i=0;i<native.size();++i)if(native[i].fd>=0){
        std::size_t index=result.queried.size();
        if(by_fd.empty()){
            // 少量不同 fd 保留有界線性搜尋，重複項目很多時也不建立雜湊表。
            const auto found=std::find_if(result.queried.begin(),result.queried.end(),
                [&](const auto& entry){return entry.fd==native[i].fd;});
            index=static_cast<std::size_t>(found-result.queried.begin());
            if(index==result.queried.size() && index==16){
                if(native.size()>by_fd.max_size()/2)throw std::length_error("socket 分組過大");
                by_fd.resize(std::bit_ceil(native.size())*2);
                for(std::size_t j=0;j<index;++j)slot(result.queried[j].fd)=j+1;
                slot(native[i].fd)=index+1;
            }
        }else{
            auto& found=slot(native[i].fd);if(!found)found=index+1;
            index=found-1;
        }
        result.indices[i]=index;
        if(index==result.queried.size()){
            result.queried.push_back(native[i]);result.handles.push_back(handles[i]);
        }else result.queried[index].events|=native[i].events;
    }
    return result;
}
}
