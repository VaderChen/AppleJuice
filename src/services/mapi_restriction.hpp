#pragma once
#include "mapi_property_tag.h"
#include "x64arm/windows_abi.hpp"
#include <unordered_set>
#include <vector>

namespace x64arm {
// 依客體 SRestriction 的聯集對齊走訪。明確保存下一個子節點，寬陣列與深鏈都不耗用宿主呼叫堆疊。
template<typename BadProperty>
bool mapi_bad_restriction(const GuestMemory& memory,unsigned unit,std::uint64_t root,BadProperty bad_property){
    enum Kind { And,Or,Not,Content,Property,Compare,Bitmask,Size,Exist,Sub,Comment,Count,Annotation };
    struct Frame {std::uint64_t node,next,remaining;};
    const auto stride=4*unit;
    const auto word=[&](std::uint64_t at){return static_cast<std::uint32_t>(memory.load(GuestAddress{at},32));};
    const auto pointer=[&](std::uint64_t at){return memory.load(GuestAddress{at},unit==4?32:64);};
    const auto readable=[&](std::uint64_t at,std::uint64_t bytes){
        if(bytes)memory.check(GuestAddress{at},bytes,Access::Read);
    };
    std::unordered_set<std::uint64_t> active,verified;
    std::vector<Frame> stack{{0,root,1}};
    while(!stack.empty()){
        auto& frame=stack.back();
        if(!frame.remaining){
            if(frame.node){active.erase(frame.node);verified.insert(frame.node);}
            stack.pop_back();
            continue;
        }
        const auto node=frame.next;
        frame.next+=stride;
        --frame.remaining;
        if(!node || active.contains(node))return true;
        if(verified.contains(node))continue;
        readable(node,stride);
        const auto kind=word(node);
        const auto data=node+unit;
        const auto first=word(data);
        std::uint64_t children=0,count=0;
        switch(kind){
        case And: case Or:
            count=first;children=pointer(data+unit);
            break;
        case Not: case Sub: case Count:
            if(kind==Not && first)return true;
            if(kind==Sub && first!=0x0e12000d && first!=0x0e13000d)return true;
            count=1;children=pointer(data+unit);
            break;
        case Annotation: case Comment:{
            children=pointer(data+unit);count=children?1:0;
            const auto props=pointer(data+2*unit);
            const auto width=mapi_property_stride(unit);
            readable(props,std::uint64_t(first)*width);
            for(std::uint32_t i=0;i<first;++i){
                const auto value=props+std::uint64_t(i)*width;
                if(bad_property(value) || (word(value)&0x1000U))return true;
            }
            break;
        }
        case Content: case Property: case Compare: case Bitmask: case Size: case Exist:{
            const auto tag=word(data+4),type=tag&0xffffU;
            if(!mapi_valid_property_tag(tag))return true;
            if(kind==Bitmask){if(first>1 || type!=3)return true;}
            else if(kind==Exist){if(first || word(data+8) || type==13)return true;}
            else if(kind==Size){if(first>5)return true;}
            else if(kind==Compare){
                if(first>6 || type!=(word(data+8)&0xffffU))return true;
                if(first==6 && type!=30 && type!=31)return true;
            }else{
                const auto scalar=type&~0x1000U;
                if(kind==Content){
                    if((first&~0x70003U) || (first&3U)>2 || (scalar!=30 && scalar!=31 && scalar!=258))return true;
                }else if(first>6 || (first==6 && scalar!=30 && scalar!=31))return true;
                const auto value=pointer(data+8);
                if(!value || bad_property(value) || (word(value)&0xffffU)!=scalar)return true;
            }
            break;
        }
        default:return true;
        }
        if(count){
            if(!children)return true;
            readable(children,count*stride);
            active.insert(node);
            stack.push_back({node,children,count});
        }else verified.insert(node);
    }
    return false;
}
}
