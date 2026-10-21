#include "x64arm/compound_file.hpp"
#include <algorithm>
#include <set>
#include <unicode/uchar.h>

namespace x64arm::compound {
bool valid_name(std::u16string_view name){
    return !name.empty() && name.size()<=31 && std::none_of(name.begin(),name.end(),[](auto c){return c==0 || c==u'/' || c==u'\\' || c==u':' || c==u'!';});
}
int compare_name(std::u16string_view left,std::u16string_view right){
    if(left.size()!=right.size())return left.size()<right.size()?-1:1;
    const auto upper=[](char16_t c){return c>=0xd800 && c<=0xdfff?static_cast<UChar32>(c):u_toupper(c);};
    for(std::size_t i=0;i<left.size();++i){const auto a=upper(left[i]),b=upper(right[i]);if(a!=b)return a<b?-1:1;}return 0;
}
std::optional<std::uint32_t> Document::find(std::uint32_t parent,std::u16string_view name)const{
    for(const auto& [id,node]:nodes)if(node.parent==parent && compare_name(node.name,name)==0)return id;
    return {};
}
std::vector<std::uint32_t> Document::children(std::uint32_t parent)const{
    std::vector<std::uint32_t> result;for(const auto& [id,node]:nodes)if(node.parent==parent)result.push_back(id);
    std::sort(result.begin(),result.end(),[&](auto a,auto b){return compare_name(nodes.at(a).name,nodes.at(b).name)<0;});return result;
}
std::uint32_t Document::add(std::uint32_t parent,std::u16string name,Kind kind){
    if(!valid_name(name))throw Error{Failure::InvalidName,"複合檔案名稱無效"};
    const auto container=nodes.find(parent);
    if(container==nodes.end())throw Error{Failure::Missing,"找不到複合檔案父儲存"};
    if(container->second.kind==Kind::Stream || (kind!=Kind::Storage && kind!=Kind::Stream))throw Error{Failure::InvalidTree,"複合檔案物件種類無效"};
    if(find(parent,name))throw Error{Failure::Duplicate,"複合檔案名稱已存在"};
    if(nodes.size()>=node_limit || next_node>=0xfffffffaU)throw Error{Failure::TooLarge,"複合檔案目錄過大"};
    const auto id=next_node;nodes.emplace(id,Node{parent,kind,std::move(name)});++next_node;return id;
}
void Document::rename(std::uint32_t id,std::u16string name){
    if(!valid_name(name))throw Error{Failure::InvalidName,"複合檔案名稱無效"};
    const auto found=nodes.find(id);if(found==nodes.end())throw Error{Failure::Missing,"找不到複合檔案物件"};
    if(!id)throw Error{Failure::InvalidTree,"複合檔案根儲存不可改名"};
    if(const auto duplicate=find(found->second.parent,name);duplicate && *duplicate!=id)throw Error{Failure::Duplicate,"複合檔案名稱已存在"};
    found->second.name=std::move(name);
}
void Document::erase(std::uint32_t id){
    if(!id)throw Error{Failure::InvalidTree,"複合檔案根儲存不可刪除"};
    if(!nodes.contains(id))throw Error{Failure::Missing,"找不到複合檔案物件"};
    std::vector<std::uint32_t> pending{id};std::set<std::uint32_t> removed;
    for(std::size_t at=0;at<pending.size();++at){
        if(!removed.insert(pending[at]).second)throw Error{Failure::InvalidTree,"複合檔案階層循環"};
        for(const auto& [child,node]:nodes)if(node.parent==pending[at])pending.push_back(child);
    }
    for(auto child:removed)nodes.erase(child);
}
void Document::validate()const{
    if(version!=3 && version!=4)throw Error{Failure::InvalidTree,"複合檔案版本無效"};
    if(nodes.empty() || nodes.size()>node_limit || !nodes.contains(0))throw Error{Failure::InvalidTree,"複合檔案缺少根儲存"};
    const auto& root=nodes.at(0);if(root.kind!=Kind::Root || root.parent!=no_node || root.name!=u"Root Entry" || !root.data.empty())throw Error{Failure::InvalidTree,"複合檔案根儲存無效"};
    std::map<std::uint32_t,std::vector<std::uint32_t>> groups;
    for(const auto& [id,node]:nodes){
        if(id>=0xfffffffaU || id>=next_node)throw Error{Failure::InvalidTree,"複合檔案目錄編號無效"};
        if(!id)continue;if(!valid_name(node.name))throw Error{Failure::InvalidName,"複合檔案名稱無效"};
        if(node.kind!=Kind::Stream && node.kind!=Kind::Storage)throw Error{Failure::InvalidTree,"複合檔案物件種類無效"};
        const auto parent=nodes.find(node.parent);if(parent==nodes.end() || parent->second.kind==Kind::Stream)throw Error{Failure::InvalidTree,"複合檔案父儲存無效"};
        if(node.kind!=Kind::Stream && !node.data.empty())throw Error{Failure::InvalidTree,"複合檔案儲存含有串流資料"};
        groups[node.parent].push_back(id);
    }
    std::set<std::uint32_t> visited;std::vector<std::uint32_t> pending{0};
    for(std::size_t at=0;at<pending.size();++at){
        const auto id=pending[at];if(!visited.insert(id).second)throw Error{Failure::InvalidTree,"複合檔案階層循環"};
        auto& group=groups[id];std::sort(group.begin(),group.end(),[&](auto a,auto b){return compare_name(nodes.at(a).name,nodes.at(b).name)<0;});
        for(std::size_t i=1;i<group.size();++i)if(compare_name(nodes.at(group[i-1]).name,nodes.at(group[i]).name)==0)throw Error{Failure::Duplicate,"複合檔案名稱已存在"};
        pending.insert(pending.end(),group.begin(),group.end());
    }
    if(visited.size()!=nodes.size())throw Error{Failure::InvalidTree,"複合檔案含有孤立或循環目錄"};
}
bool signature(std::span<const std::uint8_t> bytes)noexcept{
    constexpr std::array<std::uint8_t,8> magic{0xd0,0xcf,0x11,0xe0,0xa1,0xb1,0x1a,0xe1};
    return bytes.size()>=magic.size() && std::equal(magic.begin(),magic.end(),bytes.begin());
}
}
