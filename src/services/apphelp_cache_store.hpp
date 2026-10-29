#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include <zlib.h>

namespace x64arm::apphelp_cache {
constexpr std::uint32_t invalid=0xc000000dU,denied=0xc0000022U,missing=0xc0000225U;
constexpr std::uint32_t corrupt=0xc000003eU,quota=0xc0000044U,unsupported=0xc00000bbU;
constexpr std::uint32_t filetime_filter=0x13fU,unprivileged_update=0x5c0U;
constexpr std::size_t store_limit=4U*1024U*1024U,entry_limit=1024U;
struct Error {std::uint32_t status;};
// Windows 10 19041／19045；x86、x64 位置均由 Microsoft combase PDB 核對。
struct Layout {
    unsigned size,update,remove,driver_status,output,output_size;
    unsigned alias,file,signature,data,data_size,lookup_signature;
};
constexpr Layout layout(bool x86) {
    return x86?Layout{196,44,136,184,188,192,4,12,16,24,28,28}:
               Layout{376,80,256,352,360,368,8,24,32,48,56,48};
}
inline std::uint64_t get(std::span<const std::uint8_t> bytes,std::size_t at,unsigned width) {
    if(at>bytes.size() || width>bytes.size()-at)throw Error{corrupt};
    std::uint64_t value{};for(unsigned n=0;n<width;++n)value|=std::uint64_t{bytes[at+n]}<<(n*8);return value;
}
inline void append(std::vector<std::uint8_t>& out,std::uint64_t value,unsigned width=4) {
    if(out.size()>store_limit-width)throw Error{quota};
    for(unsigned n=0;n<width;++n)out.push_back(static_cast<std::uint8_t>(value>>(n*8)));
}
inline void append(std::vector<std::uint8_t>& out,std::span<const std::uint8_t> bytes) {
    if(bytes.size()>store_limit-out.size())throw Error{quota};
    out.insert(out.end(),bytes.begin(),bytes.end());
}
struct Record {
    std::uint32_t kind{};
    std::vector<std::uint8_t> data;
};
struct Entry {
    std::u16string key;
    std::uint64_t modified{};
    std::vector<Record> records;
};
struct Store {
    std::array<std::uint32_t,13> statistics{52};
    // 最近使用者放在尾端；key 數達上限時淘汰最舊項，總位元組超限則拒絕發布。
    std::vector<Entry> entries;
    static constexpr std::uint64_t magic=UINT64_C(0x3130304348414a41); // AJAHC001
    template<class KeyEqual>
    static Store decode(std::span<const std::uint8_t> bytes,KeyEqual equal) {
        Store result;if(bytes.empty())return result;
        if(bytes.size()>store_limit || get(bytes,0,8)!=magic)throw Error{corrupt};
        std::size_t at=8;
        const auto read=[&](unsigned width=4){const auto value=get(bytes,at,width);at+=width;return value;};
        for(auto& value:result.statistics)value=static_cast<std::uint32_t>(read());
        if(result.statistics[0]!=52)throw Error{corrupt};
        const auto count=read();if(count>entry_limit)throw Error{corrupt};
        for(std::uint64_t n=0;n<count;++n){
            Entry entry;const auto length=read();entry.modified=read(8);const auto records=read();
            if(!length || length>65534 || length%2 || records>4096)throw Error{corrupt};
            for(std::uint64_t c=0;c<length;c+=2)entry.key.push_back(static_cast<char16_t>(read(2)));
            if(std::any_of(result.entries.begin(),result.entries.end(),[&](const auto& old){return equal(old.key,entry.key);} ))throw Error{corrupt};
            for(std::uint64_t c=0;c<records;++c){
                Record record;record.kind=static_cast<std::uint32_t>(read());const auto size=read();
                if(!size || size>65536 || at>bytes.size() || size>bytes.size()-at ||
                   std::any_of(entry.records.begin(),entry.records.end(),[&](const auto& old){return old.kind==record.kind;}))throw Error{corrupt};
                const auto data=bytes.subspan(at,static_cast<std::size_t>(size));record.data.assign(data.begin(),data.end());at+=static_cast<std::size_t>(size);
                entry.records.push_back(std::move(record));
            }
            result.entries.push_back(std::move(entry));
        }
        if(at!=bytes.size())throw Error{corrupt};return result;
    }
    std::vector<std::uint8_t> encode() const {
        std::vector<std::uint8_t> out;append(out,magic,8);
        for(const auto value:statistics)append(out,value);
        append(out,entries.size());
        for(const auto& entry:entries){
            append(out,entry.key.size()*2);append(out,entry.modified,8);append(out,entry.records.size());
            for(const auto c:entry.key)append(out,c,2);
            for(const auto& record:entry.records){append(out,record.kind);append(out,record.data.size());append(out,record.data);}
        }
        return out;
    }
    static void remove(Entry& entry,std::uint32_t mask) {
        // 與 AhcpCacheEntryFilterParam 相同：整個類別均落在移除集合才移除。
        std::erase_if(entry.records,[&](const auto& record){return (record.kind&mask)==record.kind;});
    }
    template<class KeyEqual>
    Entry& acquire(std::u16string key,std::uint64_t modified,KeyEqual equal) {
        const auto found=std::find_if(entries.begin(),entries.end(),[&](const auto& entry){return equal(entry.key,key);});
        if(found==entries.end()){
            if(entries.size()==entry_limit){entries.erase(entries.begin());++statistics[11];}
            entries.push_back(Entry{std::move(key),modified,{}});++statistics[10];
        }else{
            Entry value=std::move(*found);entries.erase(found);
            if(value.modified!=modified){remove(value,filetime_filter);value.modified=modified;}
            entries.push_back(std::move(value));
        }
        return entries.back();
    }
    static std::vector<std::uint8_t> records(const Entry& entry,std::uint32_t mask,bool all=false) {
        std::vector<std::uint8_t> out;
        for(const auto& record:entry.records){
            if(!all && !(record.kind&mask))continue;
            append(out,record.kind);append(out,record.data.size());append(out,record.data);
            while(out.size()%4)append(out,0,1);
        }
        return out;
    }
    static bool sufficient(const Entry& entry,std::uint32_t mask) {
        if(entry.records.empty())return false;
        mask&=0x1e5fU;
        for(unsigned bit=0;bit<32;++bit){
            const auto kind=std::uint32_t{1}<<bit;
            if((mask&kind) && std::none_of(entry.records.begin(),entry.records.end(),[&](const auto& record){return record.kind==kind;}))return false;
        }
        return true;
    }
    // AhcStoreSave 的 st01 串流；每筆 CRC32 只涵蓋該筆 payload，不含 12 位元組標頭。
    std::vector<std::uint8_t> snapshot() const {
        std::vector<std::uint8_t> out;
        for(auto at=entries.rbegin();at!=entries.rend();++at){
            std::vector<std::uint8_t> payload;append(payload,at->key.size()*2,2);
            for(const auto c:at->key)append(payload,c,2);
            append(payload,at->modified,8);const auto data=records(*at,0,true);
            append(payload,data.size());append(payload,data);
            append(out,0x73743031U);
            append(out,::crc32(0,payload.data(),static_cast<uInt>(payload.size())));
            append(out,payload.size());append(out,payload);
        }
        return out;
    }
    void update(Entry& entry,std::uint32_t kind,std::vector<std::uint8_t> bytes) {
        if(entry.records.size()>=4096 && std::none_of(entry.records.begin(),entry.records.end(),[&](const auto& record){return record.kind==kind;}))throw Error{quota};
        std::erase_if(entry.records,[&](const auto& record){return record.kind==kind;});
        entry.records.push_back(Record{kind,std::move(bytes)});++statistics[12];
    }
};
}
