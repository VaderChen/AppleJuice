#pragma once
#include "x64arm/windows_abi.hpp"
#include <algorithm>
#include <array>
#include <vector>

namespace x64arm::bluetooth_sdp {
struct Invalid {};
struct Element {
    unsigned type{},specific{};
    std::size_t start{},body{},end{};
};
// SDP 自帶網路序的型別／長度；三個入口共用邊界與容器驗證。
class Stream {
    std::span<const std::uint8_t> bytes_;
    std::size_t remaining_{65536};
public:
    explicit Stream(std::span<const std::uint8_t> bytes):bytes_(bytes){}
    Element read(std::size_t at,std::size_t limit,unsigned depth=0){
        if(depth>64 || !remaining_-- || at>=limit || limit>bytes_.size())throw Invalid{};
        const unsigned descriptor=bytes_[at],type=descriptor>>3,size=descriptor&7;
        Element result{type,0,at,at+1,0};std::size_t count=0;
        if(type==0){if(size)throw Invalid{};}
        else if(type==1 || type==2 || type==3){
            if(size>4 || (type==3 && size!=1 && size!=2 && size!=4))throw Invalid{};
            count=std::size_t{1}<<size;result.specific=(size<<8)|(type<<4);
        }else if(type==5){if(size)throw Invalid{};count=1;}
        else if(type==4 || type==6 || type==7 || type==8){
            if(size<5)throw Invalid{};const unsigned length_bytes=1U<<(size-5);
            if(length_bytes>limit-result.body)throw Invalid{};
            for(unsigned i=0;i<length_bytes;++i)count=(count<<8)|bytes_[result.body++];
        }else throw Invalid{};
        if(count>limit-result.body)throw Invalid{};result.end=result.body+count;
        if(type==6 || type==7)for(auto cursor=result.body;cursor<result.end;)cursor=read(cursor,result.end,depth+1).end;
        return result;
    }
    std::array<std::uint8_t,24> packet(const Element& item,std::uint64_t base,unsigned unit)const{
        std::array<std::uint8_t,24> output{};
        const auto put=[&](unsigned at,unsigned width,std::uint64_t value){for(unsigned i=0;i<width;++i)output[at+i]=static_cast<std::uint8_t>(value>>(8*i));};
        put(0,4,item.type);put(4,4,item.specific);const auto count=item.end-item.body;
        if(item.type==1 || item.type==2 || item.type==3){
            if(item.type==3 && count==16){
                for(unsigned i=0;i<4;++i)output[8+i]=bytes_[item.body+3-i];
                for(unsigned i=0;i<2;++i){output[12+i]=bytes_[item.body+5-i];output[14+i]=bytes_[item.body+7-i];}
                std::copy_n(bytes_.begin()+static_cast<std::ptrdiff_t>(item.body+8),8,output.begin()+16);
            }else for(std::size_t i=0;i<count;++i)output[8+i]=bytes_[item.end-1-i];
        }else if(item.type==5)output[8]=bytes_[item.body];
        else if(item.type){const bool container=item.type==6 || item.type==7;const auto start=container?item.start:item.body;put(8,unit,base+start);put(8+unit,4,item.end-start);}
        return output;
    }
};
inline std::uint32_t invoke(GuestMemory& memory,const WindowsCall& arg,std::string_view symbol,bool x86){
    try{
        const bool single=symbol=="BluetoothSdpGetElementData",iterate=symbol=="BluetoothSdpGetContainerElementData";
        const auto base=arg(0),out=arg(single?2:3);const auto length=static_cast<std::uint32_t>(arg(1));
        if(!base || !length || !out || length>16U*1024U*1024U || base>UINT64_MAX-length || (iterate && !arg(2)))return 87;
        memory.check(GuestAddress{out},24,Access::Write);std::vector<std::uint8_t> bytes(length);memory.read(GuestAddress{base},bytes);
        Stream stream(bytes);const auto root=stream.read(0,bytes.size());auto selected=root;
        std::uint64_t next=0;const unsigned unit=x86?4:8;
        if(!single){
            if(root.type!=6 && (!iterate || root.type!=7))return 87;
            if(iterate){
                memory.check(GuestAddress{arg(2)},unit,Access::Write);
                const auto cookie=memory.load(GuestAddress{arg(2)},static_cast<std::uint8_t>(unit*8));
                const auto target=cookie?cookie:base+root.body;if(target<base+root.body || target>base+root.end)return 87;
                auto cursor=root.body;while(cursor<root.end && base+cursor<target)cursor=stream.read(cursor,root.end).end;
                if(base+cursor!=target)return 87;if(cursor==root.end)return 259;
                selected=stream.read(cursor,root.end);next=base+selected.end;
            }else{
                if(root.end!=bytes.size())return 87;bool found=false;
                for(auto cursor=root.body;cursor<root.end;){
                    const auto key=stream.read(cursor,root.end);if(key.type!=1 || key.specific!=0x110)return 87;
                    const auto value=stream.read(key.end,root.end);cursor=value.end;
                    const auto id=(unsigned{bytes[key.body]}<<8)|bytes[key.body+1];
                    if(!found && id==(arg(2)&65535)){selected=value;found=true;}
                }
                if(!found)return 2;
            }
        }
        memory.write(GuestAddress{out},stream.packet(selected,base,unit));
        if(iterate)memory.store(GuestAddress{arg(2)},static_cast<std::uint8_t>(unit*8),next);
        return 0;
    }catch(const Invalid&){return 87;}catch(const MemoryFault&){return 87;}catch(const std::bad_alloc&){return 8;}
}
}
