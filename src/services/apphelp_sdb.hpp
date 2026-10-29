#pragma once
#include "apphelp_cache_store.hpp"
#include <map>
#include <string_view>

namespace x64arm::apphelp_cache {
// 同版 Microsoft SDB 的有界讀取器；TAGID 必須指向已驗證的完整 tag。
class Sdb {
public:
    static constexpr std::size_t maximum_size=64U*1024U*1024U;
    explicit Sdb(std::span<const std::uint8_t> bytes):bytes_(bytes) {
        if(bytes.size()<42 || bytes.size()>maximum_size || get(bytes,8,4)!=0x66626473U ||
           get(bytes,0,4)<1 || get(bytes,0,4)>3)throw Error{invalid};
        scan(12,static_cast<std::uint32_t>(bytes.size()),0,0);
    }
    template<class Normalize>
    std::map<std::u16string,std::uint32_t> executable_types(Normalize normalize) const {
        const auto& first=tag(12);if(first.kind!=0x7802)throw Error{missing};
        const Tag* selected=nullptr;unsigned count=0;
        for(const auto* index:children(first.id,0x7803)){
            if(++count>32)throw Error{corrupt};
            const auto* target=child(index->id,0x3802);const auto* key=child(index->id,0x3803);
            if(target && key && number(*target)==0x7007 && number(*key)==0x4032){selected=index;break;}
        }
        if(!selected)throw Error{missing};
        const auto* bits=child(selected->id,0x9801);if(!bits)throw Error{missing};
        if(bits->size%12)throw Error{corrupt};
        const auto* flags=child(selected->id,0x4016);const bool ranges=flags && (number(*flags)&1U);
        const auto* strings=child(0,0x7801);
        std::map<std::u16string,std::uint32_t> result;std::uint64_t previous{};
        constexpr std::array<std::uint32_t,9> classes{1,0x10,0x20,0x40,0x200,0x100,2,4,0x80};
        std::size_t visited{};
        for(std::uint32_t offset=0;offset<bits->size;offset+=12){
            const auto key=get(bytes_,bits->data+offset,8);
            if(offset && (key<previous || (ranges && key==previous)))throw Error{corrupt};
            previous=key;
            if(std::find(classes.begin(),classes.end(),key)==classes.end())continue;
            const auto id=static_cast<std::uint32_t>(get(bytes_,bits->data+offset+8,4));
            const auto end=ranges && offset+12<bits->size?static_cast<std::uint32_t>(get(bytes_,bits->data+offset+20,4)):0U;
            auto at=id;const auto parent=tag(id).parent;
            if(end){const auto& stop=tag(end);if(end<=id || stop.kind!=0x7007 || stop.parent!=parent)throw Error{corrupt};}
            for(;;){
                if(++visited>1000000)throw Error{quota};
                const auto& entry=tag(at);if(entry.kind!=0x7007 || entry.parent!=parent)throw Error{corrupt};
                const auto* type=child(at,0x4032);const auto* name=child(at,0x6001);
                if(type && number(*type)==key && name){
                    if(!strings)throw Error{corrupt};
                    auto value=normalize(string(*name,*strings));
                    if(!value.empty())result[std::move(value)]|=static_cast<std::uint32_t>(key);
                    if(result.size()>100000)throw Error{quota};
                }
                if(!ranges || entry.end==end)break;
                const auto next=tags_.find(entry.end);
                if(next==tags_.end() || next->second.parent!=parent || next->second.kind!=0x7007)break;
                if(end && entry.end>end)throw Error{corrupt};
                at=entry.end;
            }
        }
        return result;
    }
    template<class Uppercase>
    std::vector<std::uint8_t> quirks_table(Uppercase uppercase) const {
        constexpr std::uint32_t bad_data=0xc00000e4U,failed=0xc0000001U;
        const auto* database=child(0,0x7001);
        const auto* library=database?child(database->id,0x7002):nullptr;
        if(!library)throw Error{bad_data};
        const auto entries=children(library->id,0x702b);
        if(entries.size()>65535)throw Error{failed};
        unsigned groups=0;
        for(const auto* entry:entries){
            const auto* group=child(entry->id,0x3002);
            if(group){const auto id=number(*group);if(id==65535)throw Error{bad_data};groups=std::max(groups,static_cast<unsigned>(id)+1);}
        }
        if(!entries.empty())groups=std::max(groups,1U);
        const auto size=24+groups*16+entries.size()*556;
        std::vector<std::uint8_t> result(size);
        const auto put=[&](std::size_t at,std::uint64_t value,unsigned width){
            if(at>result.size() || width>result.size()-at)throw Error{bad_data};
            for(unsigned n=0;n<width;++n)result[at+n]=static_cast<std::uint8_t>(value>>(8*n));
        };
        put(0,entries.size(),2);put(2,groups,2);put(4,size,4);
        const auto* strings=child(0,0x7801);
        const auto text=[&](const Tag& reference,std::size_t at){
            if(!strings)throw Error{bad_data};
            const auto& value=[&]() -> const Tag& {try{return string_tag(reference,*strings);}catch(const Error&){throw Error{bad_data};}}();
            if(value.size>256)throw Error{bad_data};
            bool terminated=false;
            for(unsigned n=0;n<value.size;n+=2)if(!get(bytes_,value.data+n,2)){terminated=true;break;}
            if(!terminated)throw Error{bad_data};
            std::copy_n(bytes_.data()+value.data,value.size,result.data()+at);
        };
        std::vector<unsigned> counts(groups);unsigned current_group=0;
        const std::size_t first=8+groups*16;
        if(groups)put(16,first,8);
        for(std::size_t index=0;index<entries.size();++index){
            const auto at=first+index*556;put(at+0x114,0xffffffffU,4);put(at+0x228,4,4);
            for(auto field=entries[index]->data;field<entries[index]->end;){
                const auto& value=tag(field);field=value.end;
                switch(value.kind){
                    case 0x6001:text(value,at);break;
                    case 0x6008:text(value,at+0x128);break;
                    case 0x501c:put(at+0x104,number(value),8);break;
                    case 0x4055:put(at+0x10c,number(value),8);break;
                    case 0x3003:case 0x3002:
                        if(number(value)==65535)throw Error{bad_data};
                        put(at+(value.kind==0x3003?0x114:0x116),number(value),2);break;
                    case 0x9010:
                        if(value.size>16)throw Error{bad_data};
                        std::copy_n(bytes_.data()+value.data,value.size,result.data()+at+0x118);break;
                    case 0x100a:put(at+0x228,0,4);break;
                    default:break;
                }
            }
            const auto id=get(result,at+0x114,2),group=get(result,at+0x116,2);
            if(id==65535 || group>=groups)throw Error{bad_data};
            if(group!=current_group){
                if(group!=current_group+1)throw Error{bad_data};
                current_group=static_cast<unsigned>(group);put(8+group*16+6,group,2);put(8+group*16+8,at,8);
            }
            if(id!=counts[group]++)throw Error{bad_data};
            put(8+group*16,counts[group],2);put(8+group*16+2,counts[group]*556,4);
            std::uint32_t hash=0;
            for(unsigned n=0;n<256;n+=2){const auto c=static_cast<char16_t>(get(result,at+n,2));if(!c)break;hash=hash*65599U+uppercase(c);}
            if(!hash)throw Error{bad_data};put(at+0x100,hash,4);
        }
        return result;
    }
private:
    struct Tag {std::uint32_t id,kind,data,size,end,parent;};
    std::span<const std::uint8_t> bytes_;
    std::map<std::uint32_t,Tag> tags_;
    void scan(std::uint32_t at,std::uint32_t end,std::uint32_t parent,unsigned depth) {
        if(depth>64)throw Error{corrupt};
        while(at<end){
            if(end-at<2 || tags_.size()>=1000000)throw Error{corrupt};
            const auto kind=static_cast<std::uint32_t>(get(bytes_,at,2)),type=kind>>12;
            std::uint32_t data=at+2,size{};
            switch(type){
                case 1:break;case 2:size=1;break;case 3:size=2;break;
                case 4:case 6:size=4;break;case 5:size=8;break;
                case 7:case 8:case 9:
                    if(end-data<4)throw Error{corrupt};size=static_cast<std::uint32_t>(get(bytes_,data,4));data+=4;break;
                default:throw Error{corrupt};
            }
            if(size>end-data)throw Error{corrupt};
            tags_.emplace(at,Tag{at,kind,data,size,data+size,parent});
            if(type==7)scan(data,data+size,at,depth+1);
            at=data+size;
        }
    }
    const Tag& tag(std::uint32_t id) const {
        const auto found=tags_.find(id);if(found==tags_.end())throw Error{corrupt};return found->second;
    }
    std::vector<const Tag*> children(std::uint32_t parent,std::uint32_t kind) const {
        std::vector<const Tag*> out;
        auto at=parent?tag(parent).data:12U;const auto end=parent?tag(parent).end:bytes_.size();
        while(at<end){const auto& value=tag(at);if(value.kind==kind)out.push_back(&value);at=value.end;}
        return out;
    }
    const Tag* child(std::uint32_t parent,std::uint32_t kind) const {
        auto at=parent?tag(parent).data:12U;const auto end=parent?tag(parent).end:bytes_.size();
        while(at<end){const auto& value=tag(at);if(value.kind==kind)return &value;at=value.end;}return nullptr;
    }
    std::uint64_t number(const Tag& value) const {return get(bytes_,value.data,value.size);}
    const Tag& string_tag(const Tag& reference,const Tag& table) const {
        const auto offset=number(reference);
        if(offset>table.end-table.id || table.end-table.id-offset<6)throw Error{corrupt};
        const auto& value=tag(table.id+static_cast<std::uint32_t>(offset));
        if(value.kind!=0x8801 || value.parent!=table.id || value.size<2 || value.size%2 || value.size>65536)throw Error{corrupt};
        return value;
    }
    std::u16string string(const Tag& reference,const Tag& table) const {
        const auto& value=string_tag(reference,table);std::u16string result;
        for(std::uint32_t n=0;n<value.size;n+=2){const auto c=static_cast<char16_t>(get(bytes_,value.data+n,2));if(!c)return result;result.push_back(c);}
        throw Error{corrupt};
    }
};

struct CompatibilityDatabase {
    static constexpr std::uint64_t magic=UINT64_C(0x3130424443414a41);
    std::array<std::uint64_t,4> identity{}; // size、mtime、file id、volume id
    std::map<std::u16string,std::uint32_t> entries;
    static CompatibilityDatabase decode(std::span<const std::uint8_t> bytes) {
        CompatibilityDatabase result;
        if(bytes.size()>store_limit || get(bytes,0,8)!=magic)throw Error{corrupt};
        std::size_t at=8;
        for(auto& value:result.identity){value=get(bytes,at,8);at+=8;}
        const auto count=get(bytes,at,4);at+=4;if(count>100000)throw Error{corrupt};
        for(std::uint64_t n=0;n<count;++n){
            const auto length=get(bytes,at,4),flags=get(bytes,at+4,4);at+=8;
            if(!length || length>65534 || length%2 || !flags || (flags&~0x3f7U))throw Error{corrupt};
            std::u16string key;
            for(std::uint64_t offset=0;offset<length;offset+=2)key.push_back(static_cast<char16_t>(get(bytes,at+offset,2)));
            at+=static_cast<std::size_t>(length);
            if(!result.entries.emplace(std::move(key),static_cast<std::uint32_t>(flags)).second)throw Error{corrupt};
        }
        if(at!=bytes.size())throw Error{corrupt};return result;
    }
    std::vector<std::uint8_t> encode() const {
        std::vector<std::uint8_t> bytes;append(bytes,magic,8);
        for(auto value:identity)append(bytes,value,8);append(bytes,entries.size());
        for(const auto& [name,flags]:entries){append(bytes,name.size()*2);append(bytes,flags);for(auto c:name)append(bytes,c,2);}
        return bytes;
    }
};

// 只持久化來源觀測及成功發布的代次；大表與客體寫入留在共用 section。
struct QuirksState {
    static constexpr std::uint64_t magic=UINT64_C(0x3130525143414a41);
    std::uint64_t observed_time{},attempt{},active{},size{};bool observed{};
    static QuirksState decode(std::span<const std::uint8_t> bytes){
        if(bytes.empty())return {};
        if(bytes.size()!=44 || get(bytes,0,8)!=magic || get(bytes,40,4)>1)throw Error{corrupt};
        QuirksState state{get(bytes,8,8),get(bytes,16,8),get(bytes,24,8),get(bytes,32,8),get(bytes,40,4)!=0};
        if(state.active>state.attempt || (state.active && (state.size<24 || state.size>24+65535U*(16U+556U))) ||
           (!state.active && state.size))throw Error{corrupt};
        return state;
    }
    std::vector<std::uint8_t> encode() const {
        std::vector<std::uint8_t> bytes;append(bytes,magic,8);append(bytes,observed_time,8);
        append(bytes,attempt,8);append(bytes,active,8);append(bytes,size,8);append(bytes,observed?1U:0U);return bytes;
    }
    std::u16string generation(std::uint64_t value) const {
        std::u16string name(16,u'0');for(unsigned n=0;n<16;++n){name[15-n]=u"0123456789abcdef"[value&15U];value>>=4;}return name;
    }
};
}
