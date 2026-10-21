#pragma once
#include "x64arm/registry.hpp"
#include "x64arm/security_descriptor.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <span>
#include <stdexcept>

namespace x64arm::registry_hive {
struct Error {std::uint32_t code{1015};};
struct Key {
    std::u16string name, class_name;
    std::map<std::u16string,RegistryValue> values;
    std::vector<std::uint8_t> security;
    std::uint64_t modified{};
    bool operator==(const Key&) const = default;
};
struct Tree {std::map<std::u16string,Key> keys;std::uint32_t minor{3};};
inline std::u16string fold(std::u16string text){for(auto& c:text)c=unicode_case_unit(c,false);return text;}
inline std::uint64_t get(std::span<const std::uint8_t> bytes,std::size_t at,unsigned width){
    if(at>bytes.size() || width>bytes.size()-at)throw Error{};
    std::uint64_t value=0;for(unsigned i=0;i<width;++i)value|=std::uint64_t(bytes[at+i])<<(i*8);return value;
}
inline void put(std::span<std::uint8_t> bytes,std::size_t at,std::uint64_t value,unsigned width){
    if(at>bytes.size() || width>bytes.size()-at)throw Error{};
    for(unsigned i=0;i<width;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));
}
inline std::u16string text(std::span<const std::uint8_t> bytes,bool compressed=false){
    if(!compressed && bytes.size()%2)throw Error{};
    std::u16string result;result.reserve(bytes.size()/(compressed?1U:2U));
    for(std::size_t at=0;at<bytes.size();at+=compressed?1U:2U)result.push_back(static_cast<char16_t>(get(bytes,at,compressed?1U:2U)));
    return result;
}
inline std::vector<std::uint8_t> text_bytes(std::u16string_view value){
    std::vector<std::uint8_t> bytes(value.size()*2);for(std::size_t i=0;i<value.size();++i)put(bytes,i*2,value[i],2);return bytes;
}
inline std::uint32_t checksum(std::span<const std::uint8_t> bytes){
    std::uint32_t value=0;for(unsigned at=0;at<508;at+=4)value^=static_cast<std::uint32_t>(get(bytes,at,4));
    return value==0?1U:value==UINT32_MAX?UINT32_MAX-1:value;
}
class Reader {
    std::span<const std::uint8_t> bytes_;
    std::map<std::uint32_t,std::span<const std::uint8_t>> cells_;
    std::set<std::uint32_t> nodes_;
    Tree result_;
    std::uint64_t value_count_{},data_size_{};
    std::span<const std::uint8_t> cell(std::uint32_t offset,std::size_t minimum=0)const{
        const auto found=cells_.find(offset);if(found==cells_.end() || found->second.size()<minimum)throw Error{};return found->second;
    }
    void subkeys(std::uint32_t offset,std::vector<std::uint32_t>& out,std::set<std::uint32_t>& lists,unsigned depth){
        if(depth>32 || !lists.insert(offset).second)throw Error{};
        const auto data=cell(offset,4);const auto signature=get(data,0,2),count=get(data,2,2);
        const unsigned stride=signature==0x666c || signature==0x686c?8U:4U;
        if((signature!=0x696c && signature!=0x666c && signature!=0x686c && signature!=0x6972) || count>(data.size()-4)/stride)throw Error{};
        for(std::uint64_t i=0;i<count;++i){const auto child=static_cast<std::uint32_t>(get(data,4+i*stride,4));
            if(signature==0x6972)subkeys(child,out,lists,depth+1);else {if(out.size()>=4096)throw Error{8};out.push_back(child);}}
    }
    void node(std::uint32_t offset,std::u16string parent,std::uint32_t parent_offset,unsigned depth){
        if(depth>512 || result_.keys.size()>=4096 || !nodes_.insert(offset).second)throw Error{};
        const auto data=cell(offset,76);if(get(data,0,2)!=0x6b6e)throw Error{};
        const auto flags=get(data,2,2),name_size=get(data,72,2);
        if(name_size>data.size()-76 || (depth && get(data,16,4)!=parent_offset))throw Error{};
        const auto name=text(data.subspan(76,name_size),(flags&0x20)!=0);
        if(name.size()>255 || (depth && name.empty()) || name.find_first_of(std::u16string{u'\\',u'\0'})!=name.npos)throw Error{};
        const auto path=depth?(parent.empty()?fold(name):parent+u"\\"+fold(name)):std::u16string{};
        if(path.size()>32768 || result_.keys.contains(path))throw Error{};
        Key key;key.name=name;key.modified=get(data,4,8);
        const auto class_size=get(data,74,2);if(class_size)key.class_name=text(cell(static_cast<std::uint32_t>(get(data,48,4)),class_size).first(class_size));
        const auto security_offset=static_cast<std::uint32_t>(get(data,44,4));
        if(security_offset!=UINT32_MAX){const auto descriptor=cell(security_offset,20);const auto length=get(descriptor,16,4);
            if(get(descriptor,0,2)!=0x6b73 || length>262144 || length>descriptor.size()-20)throw Error{};
            key.security.assign(descriptor.begin()+20,descriptor.begin()+20+static_cast<std::ptrdiff_t>(length));
            try{(void)security::parse_relative_descriptor(key.security);}catch(const SecurityDescriptorError&){throw Error{};}}
        const auto values=get(data,36,4);if(values>4096-value_count_)throw Error{8};value_count_+=values;
        if(values){const auto list=cell(static_cast<std::uint32_t>(get(data,40,4)),values*4);
            for(std::uint64_t i=0;i<values;++i){const auto value=cell(static_cast<std::uint32_t>(get(list,i*4,4)),20);if(get(value,0,2)!=0x6b76)throw Error{};
                const auto length=get(value,2,2);if(length>value.size()-20)throw Error{};
                const auto original=text(value.subspan(20,length),(get(value,16,2)&1)!=0),normalized=fold(original);
                if(original.size()>16383 || original.find(u'\0')!=original.npos || key.values.contains(normalized))throw Error{};
                RegistryValue stored;stored.type=static_cast<std::uint32_t>(get(value,12,4));if(original!=normalized)stored.display_name=original;
                const auto raw_size=get(value,4,4),size=raw_size&0x7fffffffU;
                if(size>16U*1024U*1024U-data_size_)throw Error{8};data_size_+=size;
                if(raw_size&0x80000000U){if(size>4)throw Error{};stored.bytes.assign(value.begin()+8,value.begin()+8+static_cast<std::ptrdiff_t>(size));}
                else if(size){const auto payload=cell(static_cast<std::uint32_t>(get(value,8,4)));
                    if(result_.minor>=5 && size>16344){if(payload.size()<8 || get(payload,0,2)!=0x6264)throw Error{};
                        const auto segments=get(payload,2,2);if(segments!=(size+16343)/16344)throw Error{};
                        const auto segment_list=cell(static_cast<std::uint32_t>(get(payload,4,4)),segments*4);stored.bytes.reserve(size);
                        for(std::uint64_t n=0;n<segments;++n){const auto count=std::min<std::uint64_t>(16344,size-stored.bytes.size());const auto segment=cell(static_cast<std::uint32_t>(get(segment_list,n*4,4)),count);stored.bytes.insert(stored.bytes.end(),segment.begin(),segment.begin()+static_cast<std::ptrdiff_t>(count));}}
                    else {if(size>payload.size())throw Error{};stored.bytes.assign(payload.begin(),payload.begin()+static_cast<std::ptrdiff_t>(size));}}
                key.values.emplace(normalized,std::move(stored));}}
        result_.keys.emplace(path,std::move(key));
        const auto count=get(data,20,4);if(count>4096-result_.keys.size())throw Error{8};
        if(count){std::vector<std::uint32_t> children;std::set<std::uint32_t> lists;subkeys(static_cast<std::uint32_t>(get(data,28,4)),children,lists,0);
            if(children.size()!=count)throw Error{};for(const auto child:children)node(child,path,offset,depth+1);}
    }
public:
    explicit Reader(std::span<const std::uint8_t> bytes):bytes_(bytes){}
    Tree read(){
        if(bytes_.size()<8192 || get(bytes_,0,4)!=0x66676572)throw Error{1017};
        if(bytes_.size()>32U*1024U*1024U || get(bytes_,4,4)!=get(bytes_,8,4) || get(bytes_,20,4)!=1 || get(bytes_,24,4)<2 || get(bytes_,24,4)>5 || get(bytes_,28,4) || get(bytes_,32,4)!=1 || get(bytes_,508,4)!=checksum(bytes_))throw Error{};
        result_.minor=static_cast<std::uint32_t>(get(bytes_,24,4));const auto size=get(bytes_,40,4);
        if(!size || size%4096 || size>bytes_.size()-4096)throw Error{};
        for(std::uint64_t at=0;at<size;){const auto header=bytes_.subspan(4096+at,size-at);const auto bin_size=get(header,8,4);
            if(get(header,0,4)!=0x6e696268 || get(header,4,4)!=at || bin_size<4096 || bin_size%4096 || bin_size>size-at)throw Error{};
            for(std::uint64_t position=32;position<bin_size;){const auto signed_size=static_cast<std::int32_t>(get(header,position,4));
                if(!signed_size || signed_size==INT32_MIN)throw Error{};const auto length=static_cast<std::uint32_t>(signed_size<0?-signed_size:signed_size);
                if(length<8 || length%8 || length>bin_size-position)throw Error{};
                if(signed_size<0)cells_.emplace(static_cast<std::uint32_t>(at+position),header.subspan(position+4,length-4));position+=length;}
            at+=bin_size;
        }
        node(static_cast<std::uint32_t>(get(bytes_,36,4)),{},UINT32_MAX,0);return std::move(result_);
    }
};
class Writer {
    std::vector<std::uint8_t> bytes_=std::vector<std::uint8_t>(4096);
    std::size_t bin_{},cursor_{},end_{};
    void finish_bin(){if(cursor_ && cursor_<end_)put(bytes_,cursor_,end_-cursor_,4);}
    std::uint32_t add(std::span<const std::uint8_t> data){
        const auto size=std::max<std::size_t>(8,(data.size()+11)&~std::size_t(7));
        if(!cursor_ || size>end_-cursor_){finish_bin();bin_=bytes_.size();const auto capacity=(size+32+8+4095)&~std::size_t(4095);
            if(capacity>32U*1024U*1024U-bytes_.size())throw Error{8};bytes_.resize(bin_+capacity);put(bytes_,bin_,0x6e696268,4);put(bytes_,bin_+4,bin_-4096,4);put(bytes_,bin_+8,capacity,4);cursor_=bin_+32;end_=bin_+capacity;}
        const auto offset=cursor_;put(bytes_,offset,0U-static_cast<std::uint32_t>(size),4);std::copy(data.begin(),data.end(),bytes_.begin()+static_cast<std::ptrdiff_t>(offset+4));cursor_+=size;return static_cast<std::uint32_t>(offset-4096);
    }
    void field(std::uint32_t cell,unsigned at,std::uint64_t value,unsigned width=4){put(bytes_,4096+cell+4+at,value,width);}
public:
    std::vector<std::uint8_t> write(const Tree& tree,std::uint32_t minor){
        if((minor!=3 && minor!=5) || !tree.keys.contains(u"") || tree.keys.size()>4096)throw Error{87};
        std::map<std::u16string,std::uint32_t> nodes;std::map<std::u16string,std::vector<std::u16string>> children;
        for(const auto& [path,key]:tree.keys){
            if(key.name.size()>255 || key.class_name.size()>32767 || key.name.find_first_of(std::u16string{u'\\',u'\0'})!=key.name.npos)throw Error{87};
            auto data=text_bytes(key.name);data.insert(data.begin(),76,0);put(data,0,0x6b6e,2);put(data,2,path.empty()?4:0,2);put(data,4,key.modified,8);
            for(unsigned at:{16U,28U,32U,40U,44U,48U})put(data,at,UINT32_MAX,4);put(data,72,key.name.size()*2,2);nodes[path]=add(data);
            if(!path.empty()){const auto separator=path.find_last_of(u'\\');const auto parent=separator==path.npos?std::u16string{}:path.substr(0,separator);if(!tree.keys.contains(parent))throw Error{87};children[parent].push_back(path);}
        }
        std::vector<std::uint32_t> security_nodes;std::uint64_t values=0,all_data=0;
        for(const auto& [path,key]:tree.keys){const auto nk=nodes.at(path);
            if(!path.empty()){const auto separator=path.find_last_of(u'\\');field(nk,16,nodes.at(separator==path.npos?std::u16string{}:path.substr(0,separator)));}
            if(!key.class_name.empty()){field(nk,48,add(text_bytes(key.class_name)));field(nk,74,key.class_name.size()*2,2);}
            auto descriptor=key.security;if(descriptor.empty()){descriptor.resize(20);descriptor[0]=1;put(descriptor,2,0x8004,2);}
            try{(void)security::parse_relative_descriptor(descriptor);}catch(const SecurityDescriptorError&){throw Error{87};}
            auto sk=descriptor;sk.insert(sk.begin(),20,0);put(sk,0,0x6b73,2);put(sk,12,1,4);put(sk,16,descriptor.size(),4);const auto security_offset=add(sk);security_nodes.push_back(security_offset);field(nk,44,security_offset);
            if(key.values.size()>4096-values)throw Error{8};values+=key.values.size();std::vector<std::uint8_t> list(key.values.size()*4);std::size_t index=0;
            for(const auto& [name,value]:key.values){const auto& display=value.display_name.empty()?name:value.display_name;
                if(display.size()>16383 || value.bytes.size()>16U*1024U*1024U-all_data || (minor==3 && value.bytes.size()>1024U*1024U))throw Error{87};all_data+=value.bytes.size();
                auto vk=text_bytes(display);vk.insert(vk.begin(),20,0);put(vk,0,0x6b76,2);put(vk,2,display.size()*2,2);put(vk,4,value.bytes.size(),4);put(vk,12,value.type,4);
                if(value.bytes.size()<=4){put(vk,4,value.bytes.size()|0x80000000U,4);std::copy(value.bytes.begin(),value.bytes.end(),vk.begin()+8);}
                else if(minor>=5 && value.bytes.size()>16344){const auto count=(value.bytes.size()+16343)/16344;std::vector<std::uint8_t> segments(count*4);
                    for(std::size_t i=0;i<count;++i)put(segments,i*4,add(std::span(value.bytes).subspan(i*16344,std::min<std::size_t>(16344,value.bytes.size()-i*16344))),4);
                    std::array<std::uint8_t,8> db{};put(db,0,0x6264,2);put(db,2,count,2);put(db,4,add(segments),4);put(vk,8,add(db),4);}
                else put(vk,8,add(value.bytes),4);
                put(list,index++*4,add(vk),4);field(nk,60,std::max(get(bytes_,4096+nk+4+60,4),std::uint64_t(display.size()*2)));field(nk,64,std::max(get(bytes_,4096+nk+4+64,4),std::uint64_t(value.bytes.size())));
            }
            if(!list.empty()){field(nk,36,key.values.size());field(nk,40,add(list));}
            if(const auto found=children.find(path);found!=children.end()){std::vector<std::uint8_t> links(4+found->second.size()*4);put(links,0,0x696c,2);put(links,2,found->second.size(),2);
                for(std::size_t i=0;i<found->second.size();++i){const auto& child=found->second[i];put(links,4+i*4,nodes.at(child),4);field(nk,52,std::max(get(bytes_,4096+nk+4+52,4),std::uint64_t(tree.keys.at(child).name.size()*2)));field(nk,56,std::max(get(bytes_,4096+nk+4+56,4),std::uint64_t(tree.keys.at(child).class_name.size()*2)));}
                field(nk,20,found->second.size());field(nk,28,add(links));}
        }
        for(std::size_t i=0;i<security_nodes.size();++i){field(security_nodes[i],4,security_nodes[(i+1)%security_nodes.size()]);field(security_nodes[i],8,security_nodes[(i+security_nodes.size()-1)%security_nodes.size()]);}
        finish_bin();put(bytes_,0,0x66676572,4);put(bytes_,4,1,4);put(bytes_,8,1,4);put(bytes_,12,tree.keys.at(u"").modified,8);put(bytes_,20,1,4);put(bytes_,24,minor,4);put(bytes_,32,1,4);put(bytes_,36,nodes.at(u""),4);put(bytes_,40,bytes_.size()-4096,4);put(bytes_,44,1,4);put(bytes_,508,checksum(bytes_),4);return std::move(bytes_);
    }
};
inline Tree decode(std::span<const std::uint8_t> bytes){return Reader(bytes).read();}
inline std::vector<std::uint8_t> encode(const Tree& tree,std::uint32_t minor=3){return Writer{}.write(tree,minor);}
}
