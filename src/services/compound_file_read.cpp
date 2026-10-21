#include "x64arm/compound_file.hpp"
#include <algorithm>
#include <set>

namespace x64arm::compound {
namespace {
constexpr std::uint32_t free_sector=0xffffffffU,end_chain=0xfffffffeU,fat_sector=0xfffffffdU,difat_sector=0xfffffffcU;
[[noreturn]] void invalid(){throw Error{Failure::InvalidFormat,"複合檔案格式或配置鏈無效"};}
class Reader {
    std::span<const std::uint8_t> file;
    std::size_t limit,sector_size{},sector_count{};
    std::vector<std::uint32_t> fat;
    std::vector<bool> occupied;
    std::uint64_t get(std::span<const std::uint8_t> bytes,std::size_t offset,unsigned width)const{
        if(offset>bytes.size() || width>bytes.size()-offset)invalid();
        std::uint64_t value=0;for(unsigned i=0;i<width;++i)value|=static_cast<std::uint64_t>(bytes[offset+i])<<(i*8);return value;
    }
    std::uint32_t head(std::size_t offset)const{return static_cast<std::uint32_t>(get(file,offset,4));}
    std::span<const std::uint8_t> sector(std::uint32_t id)const{
        if(id>=sector_count)invalid();return file.subspan((static_cast<std::size_t>(id)+1)*sector_size,sector_size);
    }
    void claim(std::uint32_t id){if(id>=sector_count || occupied[id])invalid();occupied[id]=true;}
    std::vector<std::uint8_t> chain(std::uint32_t first,std::optional<std::size_t> size={}){
        if(size && *size>limit)throw Error{Failure::TooLarge,"複合檔案串流超過容量限制"};
        const auto expected=size?(*size+sector_size-1)/sector_size:sector_count;
        std::vector<std::uint8_t> bytes;
        if(size)bytes.reserve(expected*sector_size);
        std::size_t count=0;
        for(auto id=first;id!=end_chain;){
            if(count>=expected || bytes.size()>limit-sector_size)invalid();claim(id);
            const auto block=sector(id);bytes.insert(bytes.end(),block.begin(),block.end());++count;id=fat.at(id);
        }
        if(size){if(count!=expected)invalid();bytes.resize(*size);}return bytes;
    }
    void read_fat(){
        const auto total=head(44),difat_count=head(72);if(!total || total>sector_count || difat_count>sector_count)invalid();
        std::vector<std::uint32_t> ids;ids.reserve(total);
        const auto add=[&](std::uint32_t id){if(ids.size()<total){if(id>=sector_count)invalid();claim(id);ids.push_back(id);}else if(id!=free_sector)invalid();};
        for(std::size_t i=0;i<109;++i)add(head(76+i*4));
        auto next=head(68);std::vector<std::uint32_t> difat;
        for(std::uint32_t i=0;i<difat_count;++i){claim(next);difat.push_back(next);const auto block=sector(next);
            for(std::size_t j=0;j<sector_size/4-1;++j)add(static_cast<std::uint32_t>(get(block,j*4,4)));
            next=static_cast<std::uint32_t>(get(block,sector_size-4,4));
        }
        if(next!=end_chain || ids.size()!=total)invalid();
        fat.reserve(ids.size()*sector_size/4);
        for(auto id:ids){const auto block=sector(id);for(std::size_t i=0;i<sector_size/4;++i)fat.push_back(static_cast<std::uint32_t>(get(block,i*4,4)));}
        if(fat.size()<sector_count)invalid();
        for(auto id:ids)if(fat[id]!=fat_sector)invalid();for(auto id:difat)if(fat[id]!=difat_sector)invalid();
    }
public:
    Reader(std::span<const std::uint8_t> bytes,std::size_t maximum):file(bytes),limit(maximum){}
    Document read(){
        if(file.size()>limit)throw Error{Failure::TooLarge,"複合檔案超過容量限制"};
        if(file.size()<512 || !signature(file))invalid();
        Document result;result.version=static_cast<std::uint16_t>(get(file,26,2));result.transaction=head(52);
        if((result.version!=3 && result.version!=4) || get(file,28,2)!=0xfffe || get(file,30,2)!=(result.version==3?9:12) || get(file,32,2)!=6 || head(56)!=4096)invalid();
        for(std::size_t i=8;i<24;++i)if(file[i])invalid();for(std::size_t i=34;i<40;++i)if(file[i])invalid();
        if(result.version==3 && head(40))invalid();
        sector_size=result.version==3?512:4096;
        if(file.size()%sector_size || file.size()<sector_size || limit<sector_size)invalid();
        sector_count=file.size()/sector_size-1;if(sector_count>=0xfffffffaU)invalid();occupied.resize(sector_count);
        read_fat();
        const auto directory=chain(head(48));if(directory.empty() || directory.size()/128>node_limit || (result.version==4 && directory.size()/sector_size!=head(40)))invalid();
        struct Entry {std::uint32_t left{},right{},child{},start{};std::uint64_t size{};bool red{};};
        std::map<std::uint32_t,Entry> entries;
        result.nodes.clear();
        for(std::size_t i=0;i<directory.size()/128;++i){
            const auto bytes=std::span(directory).subspan(i*128,128);const auto type=bytes[66];if(!type)continue;
            if((type!=1 && type!=2 && type!=5) || (type==5)!=(i==0) || bytes[67]>1)invalid();
            const auto length=get(bytes,64,2);if(length<2 || length>64 || length%2 || get(bytes,static_cast<std::size_t>(length)-2,2))invalid();
            Node node;node.kind=static_cast<Kind>(type);
            for(std::size_t j=0;j<length-2;j+=2)node.name.push_back(static_cast<char16_t>(get(bytes,j,2)));
            if((i==0 && node.name!=u"Root Entry") || (i && !valid_name(node.name)))invalid();
            std::copy_n(bytes.begin()+80,16,node.clsid.begin());node.state_bits=static_cast<std::uint32_t>(get(bytes,96,4));node.created=get(bytes,100,8);node.modified=get(bytes,108,8);
            const auto id=static_cast<std::uint32_t>(i);result.nodes.emplace(id,std::move(node));
            entries.emplace(id,Entry{static_cast<std::uint32_t>(get(bytes,68,4)),static_cast<std::uint32_t>(get(bytes,72,4)),static_cast<std::uint32_t>(get(bytes,76,4)),static_cast<std::uint32_t>(get(bytes,116,4)),get(bytes,120,result.version==3?4:8),bytes[67]==0});
            result.next_node=id+1;
        }
        if(!entries.contains(0) || entries.at(0).left!=no_node || entries.at(0).right!=no_node)invalid();
        struct Walk {std::uint32_t id,parent,lower,upper;bool red_parent;};
        std::vector<Walk> pending{{entries.at(0).child,0,no_node,no_node,false}};std::set<std::uint32_t> seen{0};
        while(!pending.empty()){
            const auto item=pending.back();pending.pop_back();if(item.id==no_node)continue;
            if(!entries.contains(item.id) || !seen.insert(item.id).second || item.id==0)invalid();
            auto& node=result.nodes.at(item.id);const auto& entry=entries.at(item.id);node.parent=item.parent;
            if((item.lower!=no_node && compare_name(node.name,result.nodes.at(item.lower).name)<=0) || (item.upper!=no_node && compare_name(node.name,result.nodes.at(item.upper).name)>=0) || (item.red_parent && entry.red))invalid();
            pending.push_back({entry.left,item.parent,item.lower,item.id,entry.red});pending.push_back({entry.right,item.parent,item.id,item.upper,entry.red});
            if(node.kind==Kind::Stream){if(entry.child!=no_node)invalid();}
            else pending.push_back({entry.child,item.id,no_node,no_node,false});
        }
        if(seen.size()!=entries.size())invalid();
        const auto mini_fat=chain(head(60),static_cast<std::size_t>(head(64))*sector_size);
        const auto& root=entries.at(0);if(root.size>limit)invalid();const auto mini_data=chain(root.start,static_cast<std::size_t>(root.size));
        std::vector<bool> used_mini((mini_data.size()+63)/64);
        for(auto& [id,node]:result.nodes){
            if(node.kind!=Kind::Stream)continue;const auto& entry=entries.at(id);if(entry.size>limit)throw Error{Failure::TooLarge,"複合檔案串流超過容量限制"};
            const auto size=static_cast<std::size_t>(entry.size);
            if(size>=4096){node.data=chain(entry.start,size);continue;}
            std::size_t count=0;auto current=entry.start;
            while(current!=end_chain){
                if(count>=(size+63)/64 || current>=used_mini.size() || used_mini[current] || current>=mini_fat.size()/4 || (static_cast<std::size_t>(current)+1)*64>mini_data.size())invalid();
                used_mini[current]=true;const auto first=mini_data.begin()+static_cast<std::ptrdiff_t>(current)*64;
                node.data.insert(node.data.end(),first,first+64);++count;current=static_cast<std::uint32_t>(get(mini_fat,static_cast<std::size_t>(current)*4,4));
            }
            if(count!=(size+63)/64)invalid();node.data.resize(size);
        }
        result.validate();return result;
    }
};
}
Document parse(std::span<const std::uint8_t> bytes,std::size_t limit){return Reader{bytes,limit}.read();}
}
