#pragma once
#include "d3d9_constant_layout.h"
#include "x64arm/memory.hpp"
#include <array>
#include <vector>
#include <algorithm>
#include <utility>

namespace x64arm {
// 裝置使用客體常駐資料；複製到 stateblock 時取值，不複製對裝置的借用。
class D3D9ConstantState {
    GuestMemory* memory_{};
    std::uint64_t address_{};
    std::array<std::array<std::vector<std::uint8_t>,3>,2> saved_;
public:
    D3D9ConstantState()=default;
    D3D9ConstantState(const D3D9ConstantState& source){
        for(unsigned stage=0;stage<2;++stage)for(unsigned kind=0;kind<3;++kind)
            saved_[stage][kind]=source.read(stage,kind);
    }
    D3D9ConstantState& operator=(const D3D9ConstantState& source){
        if(this!=&source){D3D9ConstantState copy(source);*this=std::move(copy);}return *this;
    }
    D3D9ConstantState(D3D9ConstantState&&)=default;
    D3D9ConstantState& operator=(D3D9ConstantState&&)=default;
    void bind(GuestMemory& memory,std::uint64_t address){memory_=&memory;address_=address;}
    void read_into(unsigned stage,unsigned kind,unsigned first,std::span<std::uint8_t> result)const{
        if(memory_)memory_->read(GuestAddress{address_+d9_constant_offset(stage,kind)+first},result);
        else if(const auto& saved=saved_[stage][kind];!saved.empty())std::copy_n(saved.begin()+first,result.size(),result.begin());
        else std::fill(result.begin(),result.end(),0);
    }
    std::vector<std::uint8_t> read(unsigned stage,unsigned kind,unsigned first=0,unsigned count=0)const{
        const auto size=d9_constant_limit(stage,kind)*d9_constant_stride(kind);
        if(!count)count=size-first;
        std::vector<std::uint8_t> result(count);
        read_into(stage,kind,first,result);
        return result;
    }
    void write(unsigned stage,unsigned kind,unsigned first,std::span<const std::uint8_t> bytes){
        if(memory_)memory_->write(GuestAddress{address_+d9_constant_offset(stage,kind)+first},bytes);
        else {auto& saved=saved_[stage][kind];if(saved.empty())saved.resize(d9_constant_limit(stage,kind)*d9_constant_stride(kind));std::copy(bytes.begin(),bytes.end(),saved.begin()+first);}
    }
    void clear(){
        if(memory_)memory_->fill(GuestAddress{address_},0,D9_CONSTANT_BYTES);
        else for(auto& stage:saved_)for(auto& values:stage)values.clear();
    }
};
}
