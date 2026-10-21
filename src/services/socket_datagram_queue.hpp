#pragma once
#include "socket_multicast_filter.hpp"
#include <limits>
#include <sys/stat.h>

namespace x64arm::socket_detail {
// 固定寬度、無指標的資料報紀錄。控制資料是宿主 ABI，客體匯出另經 codec。
struct DatagramRecord {
    std::vector<std::uint8_t> name,control,data;
    std::uint32_t flags{};
};
struct DatagramSnapshot {
    std::uint64_t packets{},payload{},occupied{},capacity{};
};

// 在既有純量狀態頁後附加環形佇列；prefix 不改寫。每次使用須持有同一
// state descriptor 的 MulticastAccess，跨 PID 以 record lock 序列化。
class DatagramQueue {
    MulticastAccess access_;
    int fd_;
    std::uint64_t base_;
    bool valid_{};
    struct Header {
        std::uint64_t sequence{},capacity{},head{},tail{},used{},payload{},packets{};
        std::uint32_t format{2};
    } header_;
    static constexpr std::uint64_t magic=UINT64_C(0x3151455547443458);
    static constexpr std::uint64_t layout_magic=UINT64_C(0x3259414c47443458);
    static constexpr std::size_t header_size=80,record_size=32;
    static constexpr std::size_t layout_size=32;
    static constexpr std::uint64_t max_capacity=64*1024*1024;
    static std::uint64_t checksum(const std::uint8_t* bytes,std::size_t size,std::uint64_t value=UINT64_C(14695981039346656037)){
        for(std::size_t i=0;i<size;++i){value^=bytes[i];value*=UINT64_C(1099511628211);}return value;
    }
    static void put(std::uint8_t* bytes,unsigned size,std::uint64_t value){for(unsigned i=0;i<size;++i)bytes[i]=static_cast<std::uint8_t>(value>>(i*8));}
    static std::uint64_t get(const std::uint8_t* bytes,unsigned size){std::uint64_t value=0;for(unsigned i=0;i<size;++i)value|=std::uint64_t(bytes[i])<<(i*8);return value;}
    static bool transfer_at(int fd,void* bytes,std::size_t size,std::uint64_t offset,bool writing) {
        auto* data=static_cast<std::uint8_t*>(bytes);
        while(size){const auto count=writing?::pwrite(fd,data,size,static_cast<off_t>(offset)) : ::pread(fd,data,size,static_cast<off_t>(offset));
            if(count<0 && errno==EINTR)continue;if(count<=0){if(!count)errno=EPROTO;return false;}
            data+=count;size-=static_cast<std::size_t>(count);offset+=static_cast<std::size_t>(count);
        }return true;
    }
    bool transfer(void* bytes,std::size_t size,std::uint64_t offset,bool writing)const{return transfer_at(fd_,bytes,size,offset,writing);}
    bool ring(void* bytes,std::size_t size,std::uint64_t position,bool writing) const {
        const auto first=std::min<std::uint64_t>(size,header_.capacity-position);
        return transfer(bytes,first,base_+2*header_size+position,writing) &&
            (first==size || transfer(static_cast<std::uint8_t*>(bytes)+first,size-first,base_+2*header_size,writing));
    }
    static bool decode(const std::array<std::uint8_t,header_size>& bytes,Header& header){
        const auto format=get(bytes.data()+8,4);
        if(get(bytes.data(),8)!=magic || (format!=1 && format!=2) || get(bytes.data()+12,4)!=header_size ||
           checksum(bytes.data(),72)!=get(bytes.data()+72,8))return false;
        // Header 只在程序內使用；序列化逐欄位，沒有依賴其 padding 或指標寬度。
        header={get(bytes.data()+16,8),get(bytes.data()+24,8),get(bytes.data()+32,8),get(bytes.data()+40,8),get(bytes.data()+48,8),get(bytes.data()+56,8),get(bytes.data()+64,8),static_cast<std::uint32_t>(format)};
        return header.sequence && header.capacity>=1024 && header.capacity<=max_capacity &&
            header.head<header.capacity && header.tail<header.capacity && header.used<=header.capacity &&
            header.tail==(header.head+header.used)%header.capacity && header.payload<=header.used &&
            header.packets<=header.used/record_size && ((!header.packets)==(!header.used));
    }
    bool commit(Header next){
        if(header_.sequence==UINT64_MAX){errno=EOVERFLOW;return false;}next.sequence=header_.sequence+1;
        std::array<std::uint8_t,header_size> bytes{};put(bytes.data(),8,magic);put(bytes.data()+8,4,next.format);put(bytes.data()+12,4,header_size);
        const std::array<std::uint64_t,7> values{next.sequence,next.capacity,next.head,next.tail,next.used,next.payload,next.packets};
        for(unsigned i=0;i<values.size();++i)put(bytes.data()+16+8*i,8,values[i]);put(bytes.data()+72,8,checksum(bytes.data(),72));
        // 紀錄完整寫入後才發布下一個 header；另一份 header 保留前次已提交狀態。
        if(!transfer(bytes.data(),bytes.size(),base_+((next.sequence-1)%2)*header_size,true))return false;
        header_=next;return true;
    }
    static bool record_length(const DatagramRecord& record,std::size_t& size){
        if(record.name.size()>sizeof(sockaddr_storage) || record.control.size()>65536 || record.data.size()>65536){errno=EMSGSIZE;return false;}
        size=(record_size+record.name.size()+record.control.size()+record.data.size()+7)&~std::size_t(7);return true;
    }
    bool front(DatagramRecord& record,std::size_t& size){
        if(!valid_){errno=EPROTO;return false;}if(!header_.packets){errno=EAGAIN;return false;}
        std::array<std::uint8_t,record_size> prefix{};if(!ring(prefix.data(),prefix.size(),header_.head,false))return false;
        size=get(prefix.data(),4);const auto name=get(prefix.data()+8,4),control=get(prefix.data()+12,4),data=get(prefix.data()+16,4);
        if(name>sizeof(sockaddr_storage) || control>65536 || data>65536 || get(prefix.data()+20,4) ||
           size!=((record_size+name+control+data+7)&~UINT64_C(7)) || size>header_.used || data>header_.payload ||
           (header_.packets==1 && (size!=header_.used || data!=header_.payload))){errno=EPROTO;return false;}
        std::vector<std::uint8_t> bytes(size);if(!ring(bytes.data(),bytes.size(),header_.head,false))return false;
        const auto sum=checksum(bytes.data()+record_size,size-record_size,checksum(bytes.data(),24));
        if(sum!=get(bytes.data()+24,8)){errno=EPROTO;return false;}
        const auto name_count=static_cast<std::ptrdiff_t>(name),control_count=static_cast<std::ptrdiff_t>(control),data_count=static_cast<std::ptrdiff_t>(data);
        auto begin=bytes.begin()+record_size;record.name.assign(begin,begin+name_count);begin+=name_count;
        record.control.assign(begin,begin+control_count);begin+=control_count;record.data.assign(begin,begin+data_count);record.flags=static_cast<std::uint32_t>(get(bytes.data()+4,4));return true;
    }
public:
    static constexpr std::uint64_t default_base=4096;
    // 不變的格式與容量可在跨程序匯入時讀取，不必等待收包鎖；可變游標
    // 與紀錄的完整校驗仍在真正的 queue 操作中完成。
    static bool inspect_layout(int fd,std::uint64_t base=default_base){
        struct stat info{};if(::fstat(fd,&info)<0)return false;
        if(!S_ISREG(info.st_mode) || info.st_size<0 || base<256 || base>1024*1024 || base%8 || std::uint64_t(info.st_size)<base+2*header_size+1024){errno=EPROTO;return false;}
        for(unsigned slot=0;slot<2;++slot){std::array<std::uint8_t,32> bytes{};
            if(!transfer_at(fd,bytes.data(),bytes.size(),base+slot*header_size,false))return false;
            const auto format=get(bytes.data()+8,4),capacity=get(bytes.data()+24,8);
            if(get(bytes.data(),8)!=magic || (format!=1 && format!=2) || get(bytes.data()+12,4)!=header_size || capacity<1024 || capacity>max_capacity)continue;
            const auto end=base+2*header_size+capacity;
            if(format==1 && std::uint64_t(info.st_size)==end)return true;
            if(format==2 && std::uint64_t(info.st_size)==end+layout_size){std::array<std::uint8_t,layout_size> layout{};
                if(!transfer_at(fd,layout.data(),layout.size(),end,false))return false;
                if(get(layout.data(),8)==layout_magic && get(layout.data()+8,4)==2 && get(layout.data()+12,4)==header_size && get(layout.data()+16,8)==capacity && checksum(layout.data(),24)==get(layout.data()+24,8))return true;
            }
        }errno=EPROTO;return false;
    }
    // 單檔上限有界；滿佇列回 ENOBUFS，保留尚未讀取的原生封包。
    explicit DatagramQueue(int state_fd,std::uint64_t base=default_base,std::uint64_t requested_capacity=0):access_(state_fd),fd_(state_fd),base_(base){
        if(!access_)return;
        if(base<256 || base>1024*1024 || (base%8) || (requested_capacity && (requested_capacity<1024 || requested_capacity>max_capacity))){errno=EINVAL;return;}
        struct stat info{};if(::fstat(fd_,&info)<0)return;if(!S_ISREG(info.st_mode) || info.st_size<0){errno=EINVAL;return;}
        if(std::uint64_t(info.st_size)<=base_){
            header_.capacity=requested_capacity?requested_capacity:2*1024*1024;
            const auto end=base_+2*header_size+header_.capacity;
            std::array<std::uint8_t,layout_size> layout{};put(layout.data(),8,layout_magic);put(layout.data()+8,4,2);put(layout.data()+12,4,header_size);put(layout.data()+16,8,header_.capacity);put(layout.data()+24,8,checksum(layout.data(),24));
            if(::ftruncate(fd_,static_cast<off_t>(end+layout_size))<0 || !transfer(layout.data(),layout.size(),end,true) || !commit(header_))return;
        }else{
            std::array<std::uint8_t,header_size> first{},second{};Header a{},b{};
            if(!transfer(first.data(),first.size(),base_,false) || !transfer(second.data(),second.size(),base_+header_size,false))return;
            const bool good_a=decode(first,a),good_b=decode(second,b);
            if(!good_a && !good_b){errno=EPROTO;return;}
            header_=good_a && (!good_b || a.sequence>b.sequence)?a:b;
            if(!inspect_layout(fd_,base_) || std::uint64_t(info.st_size)!=base_+2*header_size+header_.capacity+(header_.format==2?layout_size:0) || (requested_capacity && requested_capacity!=header_.capacity)){errno=EPROTO;return;}
        }valid_=true;
    }
    DatagramQueue(const DatagramQueue&)=delete;
    DatagramQueue& operator=(const DatagramQueue&)=delete;
    explicit operator bool()const{return valid_;}
    DatagramSnapshot snapshot()const{return {header_.packets,header_.payload,header_.used,header_.capacity};}
    bool fits(const DatagramRecord& record)const{if(!valid_){errno=EPROTO;return false;}std::size_t size;if(!record_length(record,size))return false;if(size>header_.capacity-header_.used){errno=ENOBUFS;return false;}return true;}
    bool push(const DatagramRecord& record){
        if(!valid_){errno=EPROTO;return false;}if(!fits(record))return false;std::size_t size;if(!record_length(record,size))return false;
        std::vector<std::uint8_t> bytes(size);put(bytes.data(),4,size);put(bytes.data()+4,4,record.flags);put(bytes.data()+8,4,record.name.size());put(bytes.data()+12,4,record.control.size());put(bytes.data()+16,4,record.data.size());
        auto next=bytes.begin()+record_size;next=std::copy(record.name.begin(),record.name.end(),next);next=std::copy(record.control.begin(),record.control.end(),next);std::copy(record.data.begin(),record.data.end(),next);
        put(bytes.data()+24,8,checksum(bytes.data()+record_size,size-record_size,checksum(bytes.data(),24)));
        if(!ring(bytes.data(),bytes.size(),header_.tail,true))return false;
        auto header=header_;header.tail=(header.tail+size)%header.capacity;header.used+=size;header.payload+=record.data.size();++header.packets;return commit(header);
    }
    bool peek(DatagramRecord& record){std::size_t size;return front(record,size);}
    bool pop(DatagramRecord& record){
        std::size_t size;if(!front(record,size))return false;auto header=header_;
        header.head=(header.head+size)%header.capacity;header.used-=size;header.payload-=record.data.size();--header.packets;
        if(!header.packets)header.head=header.tail=0;return commit(header);
    }
};
} // namespace x64arm::socket_detail
