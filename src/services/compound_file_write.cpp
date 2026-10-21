#include "x64arm/compound_file.hpp"
#include <algorithm>
#include <functional>

namespace x64arm::compound {
namespace {
constexpr std::uint32_t free_sector=0xffffffffU,end_chain=0xfffffffeU,fat_sector=0xfffffffdU,difat_sector=0xfffffffcU;
void put(std::span<std::uint8_t> bytes,std::size_t offset,std::uint64_t value,unsigned width){
    for(unsigned i=0;i<width;++i)bytes[offset+i]=static_cast<std::uint8_t>(value>>(i*8));
}
class Writer {
    const Document& document;std::size_t limit,sector_size;
    std::vector<std::uint8_t> body;std::vector<std::uint32_t> fat;
    [[noreturn]] void large()const{throw Error{Failure::TooLarge,"複合檔案超過容量限制"};}
    std::uint32_t allocate(std::size_t size){
        const auto count=(size+sector_size-1)/sector_size;
        if(!count)return end_chain;
        if(count>=0xfffffffaU || fat.size()+count>=0xfffffffaU || count>(limit-sector_size-body.size())/sector_size)large();
        const auto first=static_cast<std::uint32_t>(fat.size());body.resize(body.size()+count*sector_size);
        for(std::size_t i=0;i<count;++i)fat.push_back(i+1<count?first+static_cast<std::uint32_t>(i)+1:end_chain);return first;
    }
    std::uint32_t chain(std::span<const std::uint8_t> bytes){const auto first=allocate(bytes.size());if(!bytes.empty())std::copy(bytes.begin(),bytes.end(),body.begin()+static_cast<std::ptrdiff_t>(first)*static_cast<std::ptrdiff_t>(sector_size));return first;}
public:
    Writer(const Document& value,std::size_t maximum,std::span<const std::uint8_t> previous={}):document(value),limit(maximum),sector_size(value.version==3?512:4096){
        if(previous.empty())return;
        if(previous.size()>limit)large();
        if(previous.size()<sector_size || previous.size()%sector_size || !signature(previous) || previous[26]!=value.version || previous[27])throw Error{Failure::InvalidFormat,"前一代複合檔案格式無效"};
        body.assign(previous.begin()+static_cast<std::ptrdiff_t>(sector_size),previous.end());fat.resize(body.size()/sector_size,free_sector);
    }
    std::vector<std::uint8_t> write(){
        document.validate();if(limit<sector_size)large();
        std::map<std::uint32_t,std::uint32_t> wire;std::vector<std::uint32_t> ids;ids.reserve(document.nodes.size());
        for(const auto& [id,node]:document.nodes){wire.emplace(id,static_cast<std::uint32_t>(ids.size()));ids.push_back(id);}
        const auto directory_start=allocate(ids.size()*128);const auto directory_sectors=fat.size()-directory_start;
        std::vector<std::uint8_t> mini;std::vector<std::uint32_t> mini_fat;
        struct Link {std::uint32_t left{no_node},right{no_node},child{no_node},start{end_chain};};
        std::vector<Link> links(ids.size());
        for(const auto& [id,node]:document.nodes){
            if(node.kind!=Kind::Stream)continue;
            if(node.data.size()>limit || (document.version==3 && node.data.size()>UINT32_MAX))large();
            auto& entry=links[wire.at(id)];if(node.data.empty())continue;
            if(node.data.size()>=4096){entry.start=chain(node.data);continue;}
            const auto count=(node.data.size()+63)/64,first=mini_fat.size();if(first+count>limit/64)large();
            entry.start=static_cast<std::uint32_t>(first);mini.resize((first+count)*64);
            std::copy(node.data.begin(),node.data.end(),mini.begin()+static_cast<std::ptrdiff_t>(first)*64);
            for(std::size_t i=0;i<count;++i)mini_fat.push_back(i+1<count?static_cast<std::uint32_t>(first+i+1):end_chain);
        }
        links[0].start=chain(mini);
        const auto mini_fat_count=(mini_fat.size()*4+sector_size-1)/sector_size;
        std::vector<std::uint8_t> mini_fat_bytes(mini_fat_count*sector_size,0xff);
        for(std::size_t i=0;i<mini_fat.size();++i)put(mini_fat_bytes,i*4,mini_fat[i],4);
        const auto mini_fat_start=chain(mini_fat_bytes);
        std::function<std::uint32_t(const std::vector<std::uint32_t>&,std::size_t,std::size_t)> tree;
        tree=[&](const auto& children,std::size_t begin,std::size_t end)->std::uint32_t{
            if(begin==end)return no_node;const auto middle=begin+(end-begin)/2;const auto index=wire.at(children[middle]);
            links[index].left=tree(children,begin,middle);links[index].right=tree(children,middle+1,end);return index;
        };
        for(const auto& [id,node]:document.nodes)if(node.kind!=Kind::Stream){const auto children=document.children(id);links[wire.at(id)].child=tree(children,0,children.size());}
        auto directory=std::span(body).subspan(static_cast<std::size_t>(directory_start)*sector_size,directory_sectors*sector_size);
        for(std::size_t i=0;i<directory.size()/128;++i){auto entry=directory.subspan(i*128,128);put(entry,68,no_node,4);put(entry,72,no_node,4);put(entry,76,no_node,4);
            if(i>=ids.size())continue;const auto& node=document.nodes.at(ids[i]);const auto& link=links[i];
            for(std::size_t j=0;j<node.name.size();++j)put(entry,j*2,node.name[j],2);put(entry,64,(node.name.size()+1)*2,2);
            put(entry,66,static_cast<unsigned>(node.kind),1);put(entry,67,1,1);put(entry,68,link.left,4);put(entry,72,link.right,4);put(entry,76,link.child,4);
            if(node.kind!=Kind::Stream){std::copy(node.clsid.begin(),node.clsid.end(),entry.begin()+80);put(entry,100,i?node.created:0,8);put(entry,108,node.modified,8);}
            put(entry,96,node.state_bits,4);put(entry,116,node.kind==Kind::Storage?0:link.start,4);put(entry,120,node.kind==Kind::Root?mini.size():node.kind==Kind::Stream?node.data.size():0,8);
        }
        const auto base=fat.size(),per_fat=sector_size/4,per_difat=per_fat-1;
        std::size_t fat_count=0,difat_count=0;
        for(;;){const auto next_fat=(base+fat_count+difat_count+per_fat-1)/per_fat,next_difat=next_fat>109?(next_fat-109+per_difat-1)/per_difat:0;
            if(fat_count==next_fat && difat_count==next_difat)break;fat_count=next_fat;difat_count=next_difat;}
        if(fat_count+difat_count>(limit-sector_size-body.size())/sector_size)large();
        body.resize((base+fat_count+difat_count)*sector_size);fat.insert(fat.end(),fat_count,fat_sector);fat.insert(fat.end(),difat_count,difat_sector);
        for(std::size_t i=0;i<fat_count;++i){auto bytes=std::span(body).subspan((base+i)*sector_size,sector_size);std::fill(bytes.begin(),bytes.end(),0xff);
            for(std::size_t j=0;j<per_fat && i*per_fat+j<fat.size();++j)put(bytes,j*4,fat[i*per_fat+j],4);}
        for(std::size_t i=0;i<difat_count;++i){auto bytes=std::span(body).subspan((base+fat_count+i)*sector_size,sector_size);std::fill(bytes.begin(),bytes.end(),0xff);
            for(std::size_t j=0;j<per_difat && 109+i*per_difat+j<fat_count;++j)put(bytes,j*4,base+109+i*per_difat+j,4);
            put(bytes,sector_size-4,i+1<difat_count?base+fat_count+i+1:end_chain,4);}
        std::vector<std::uint8_t> output(sector_size);
        constexpr std::array<std::uint8_t,8> magic{0xd0,0xcf,0x11,0xe0,0xa1,0xb1,0x1a,0xe1};std::copy(magic.begin(),magic.end(),output.begin());
        put(output,24,0x3e,2);put(output,26,document.version,2);put(output,28,0xfffe,2);put(output,30,document.version==3?9:12,2);put(output,32,6,2);
        put(output,40,document.version==3?0:directory_sectors,4);put(output,44,fat_count,4);put(output,48,directory_start,4);put(output,52,document.transaction,4);
        put(output,56,4096,4);put(output,60,mini_fat_start,4);put(output,64,mini_fat_count,4);put(output,68,difat_count?base+fat_count:end_chain,4);put(output,72,difat_count,4);
        for(std::size_t i=0;i<109;++i)put(output,76+i*4,i<fat_count?base+i:free_sector,4);
        output.insert(output.end(),body.begin(),body.end());return output;
    }
};
}
std::vector<std::uint8_t> serialize(const Document& document,std::size_t limit){return Writer{document,limit}.write();}
std::vector<std::uint8_t> serialize_append(const Document& document,std::span<const std::uint8_t> previous,std::size_t limit){return Writer{document,limit,previous}.write();}
}
