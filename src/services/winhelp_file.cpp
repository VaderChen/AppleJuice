#include "winhelp_file.hpp"
#include <algorithm>
#include <stdexcept>
#include <set>

namespace x64arm::winhelp {
namespace {
[[noreturn]] void bad(){throw std::invalid_argument("HLP 容器或系統資訊損毀");}
struct Bytes {
    std::span<const std::uint8_t> data;
    std::span<const std::uint8_t> part(std::size_t at,std::size_t size)const{if(at>data.size() || size>data.size()-at)bad();return data.subspan(at,size);}
    std::uint16_t u16(std::size_t at)const{const auto p=part(at,2);return static_cast<std::uint16_t>(p[0]|unsigned(p[1])<<8);}
    std::uint32_t u32(std::size_t at)const{const auto p=part(at,4);return std::uint32_t(p[0])|std::uint32_t(p[1])<<8|std::uint32_t(p[2])<<16|std::uint32_t(p[3])<<24;}
    std::string string(std::size_t& at)const{const auto begin=at;while(at<data.size() && data[at])++at;if(at==data.size())bad();std::string result(data.begin()+static_cast<std::ptrdiff_t>(begin),data.begin()+static_cast<std::ptrdiff_t>(at));++at;return result;}
};
System system(std::span<const std::uint8_t> bytes){
    const Bytes b{bytes};if(b.u16(0)!=0x036c || b.u16(4)!=1)bad();System result;result.version=b.u16(2);result.flags=b.u16(10);
    std::size_t at=12;if(result.version<=16){result.title=b.string(at);return result;}
    while(at<bytes.size()){
        const auto kind=b.u16(at),size=b.u16(at+2);at+=4;const Bytes value{b.part(at,size)};at+=size;
        if(kind==1 || kind==2 || kind==4){std::size_t pos=0;auto text=value.string(pos);if(pos!=size)bad();if(kind==1)result.title=std::move(text);else if(kind==2)result.copyright=std::move(text);else result.macros.push_back(std::move(text));}
        else if(kind==3){if(size!=4)bad();result.contents=value.u32(0);}
        else if(kind==6){if(size!=90)bad();WindowSpec window;window.flags=value.u16(0);
            const auto fixed=[&](std::size_t offset,std::size_t length){const auto data=value.part(offset,length);return std::string(data.begin(),std::find(data.begin(),data.end(),0));};
            window.type=fixed(2,10);window.name=fixed(12,9);window.caption=fixed(21,51);
            window.x=static_cast<std::int16_t>(value.u16(72));window.y=static_cast<std::int16_t>(value.u16(74));window.width=static_cast<std::int16_t>(value.u16(76));window.height=static_cast<std::int16_t>(value.u16(78));window.maximize=static_cast<std::int16_t>(value.u16(80));window.scroll_color=value.u32(82);window.non_scroll_color=value.u32(86);result.windows.push_back(std::move(window));}
    }
    return result;
}
}
File read(std::span<const std::uint8_t> bytes){
    if(bytes.size()>256U*1024U*1024U)bad();const Bytes file{bytes};if(file.u32(0)!=0x00035f3f || file.u32(12)!=bytes.size())bad();
    const auto stream=[&](std::uint32_t offset){if(offset<16)bad();const auto reserved=file.u32(offset),used=file.u32(std::size_t(offset)+4);if(reserved<9 || used>reserved-9)bad();(void)file.part(offset,reserved);return file.part(std::size_t(offset)+9,used);};
    const auto directory=file.u32(4);const Bytes tree{stream(directory)};
    if(tree.u16(0)!=0x293b || (tree.u16(2)&0x402)!=0x402 || tree.u16(22)!=0 || tree.u16(28)!=0xffff)bad();
    if(tree.data.size()<38 || tree.data[6]!='z' || tree.data[7]!='4' || tree.data[8]!=0)bad();
    const unsigned page_size=tree.u16(4),count=tree.u16(30),levels=tree.u16(32);const auto entries=tree.u32(34);
    if(page_size<8 || !count || !levels || levels>32 || entries>100000)bad();(void)tree.part(38,std::size_t(count)*page_size);
    File result;std::set<unsigned> visited;std::vector<unsigned> leaves;std::vector<std::pair<unsigned,unsigned>> links;
    std::set<std::uint32_t> offsets{directory};std::size_t payload{};std::string previous_name;
    const auto visit=[&](const auto& self,unsigned index,unsigned depth)->void{
        if(index>=count || !visited.insert(index).second)bad();const Bytes page{tree.part(38+std::size_t(index)*page_size,page_size)};
        const unsigned unused=page.u16(0),n=page.u16(2),header=depth==1?8:6;if(unused>page_size-header)bad();const Bytes used{page.data.first(page_size-unused)};
        std::size_t at=header;
        if(depth>1){self(self,page.u16(4),depth-1);std::string previous;
            for(unsigned i=0;i<n;++i){auto name=used.string(at);if(name.empty() || (!previous.empty() && name<=previous))bad();previous=std::move(name);const auto child=used.u16(at);at+=2;self(self,child,depth-1);}
        }else{
            leaves.push_back(index);links.emplace_back(page.u16(4),page.u16(6));
            for(unsigned i=0;i<n;++i){auto name=used.string(at);if(name.empty() || (!previous_name.empty() && name<=previous_name))bad();previous_name=name;const auto offset=used.u32(at);at+=4;if(!offsets.insert(offset).second)bad();const auto value=stream(offset);if(value.size()>bytes.size()-payload)bad();payload+=value.size();result.streams.emplace(std::move(name),std::vector<std::uint8_t>(value.begin(),value.end()));}
        }
        if(at!=used.data.size())bad();
    };
    visit(visit,tree.u16(26),levels);if(result.streams.size()!=entries)bad();
    for(std::size_t i=0;i<leaves.size();++i)if(links[i].first!=(i?leaves[i-1]:0xffffU) || links[i].second!=(i+1<leaves.size()?leaves[i+1]:0xffffU))bad();
    const auto found=result.streams.find("|SYSTEM");if(found==result.streams.end())bad();result.system=system(found->second);return result;
}
std::vector<std::uint8_t> decompress(std::span<const std::uint8_t> input,std::size_t limit,bool padding){
    if(limit>256U*1024U*1024U)bad();std::vector<std::uint8_t> out;std::size_t at=0;
    while(at<input.size()){
        const auto control=input[at++];if(at==input.size()){if(padding && !control)return out;bad();}
        for(unsigned bit=0;bit<8 && at<input.size();++bit){
            if(out.size()==limit){if(padding && !(control>>bit) && std::all_of(input.begin()+static_cast<std::ptrdiff_t>(at),input.end(),[](auto byte){return !byte;}))return out;bad();}
            if(!(control&(1U<<bit)))out.push_back(input[at++]);
            else{if(input.size()-at<2)bad();const unsigned value=input[at]|unsigned(input[at+1])<<8;at+=2;const unsigned distance=(value&0xfffU)+1,length=(value>>12)+3;if(distance>out.size() || length>limit-out.size())bad();for(unsigned i=0;i<length;++i)out.push_back(out[out.size()-distance]);}
        }
    }
    return out;
}
std::vector<TopicBlock> topic_blocks(const File& file){
    const auto found=file.streams.find("|TOPIC");if(found==file.streams.end())bad();const auto& bytes=found->second;
    const bool old=file.system.version<=16;const auto flags=old?0:file.system.flags;if(flags!=0 && flags!=4 && flags!=8)bad();
    const std::size_t size=old || flags==8?2048:4096,limit=flags?16384:size-12;std::vector<TopicBlock> blocks;std::size_t total=0;
    for(std::size_t at=0;at<bytes.size();at+=size){const Bytes block{std::span{bytes}.subspan(at,std::min(size,bytes.size()-at))};const auto data=block.part(12,block.data.size()>=12?block.data.size()-12:0);
        TopicBlock value{block.u32(0),block.u32(4),block.u32(8),flags?decompress(data,limit,true):std::vector<std::uint8_t>(data.begin(),data.end())};
        if(value.data.size()>256U*1024U*1024U-total)bad();total+=value.data.size();blocks.push_back(std::move(value));}
    return blocks;
}
std::vector<TopicRecord> topic_records(const File& file){
    const auto blocks=topic_blocks(file);if(blocks.empty())return {};const bool old=file.system.version<=16;
    // Win32 TOPICPOS 的區塊步距固定 16 KiB，未壓縮的實體區塊仍只有 4 KiB。
    const std::size_t capacity=old?2036:16384;
    const auto copy=[&](std::uint32_t position,std::size_t size){
        if(position<12 || size>256U*1024U*1024U)bad();std::size_t page=(position-12)/capacity,offset=(position-12)%capacity;std::vector<std::uint8_t> out;
        while(size){if(page>=blocks.size() || offset>=blocks[page].data.size())bad();const auto n=std::min(size,blocks[page].data.size()-offset);out.insert(out.end(),blocks[page].data.begin()+static_cast<std::ptrdiff_t>(offset),blocks[page].data.begin()+static_cast<std::ptrdiff_t>(offset+n));size-=n;++page;offset=0;}return out;
    };
    std::vector<TopicRecord> records;std::set<std::uint32_t> visited;auto position=blocks.front().first;std::uint32_t previous=UINT32_MAX;std::size_t total=0;
    while(position && position!=UINT32_MAX){
        if(records.size()>=100000 || !visited.insert(position).second || position<12)bad();const auto page=(position-12)/capacity,offset=(position-12)%capacity;if(page>=blocks.size() || offset>blocks[page].data.size() || blocks[page].data.size()-offset<21)bad();const auto raw=copy(position,21);const Bytes header{raw};const auto size=header.u32(0),first=header.u32(16);if(first<21 || size<first || size>256U*1024U*1024U-total)bad();total+=size;
        TopicRecord record;record.position=position;record.previous=header.u32(8);record.next=header.u32(12);record.text_size=header.u32(4);record.kind=raw[20];if(record.text_size>256U*1024U*1024U)bad();
        if(!records.empty() && (old?record.previous>position || position-record.previous!=previous:record.previous!=previous))bad();
        const auto data=copy(position,size);record.formatting.assign(data.begin()+21,data.begin()+first);record.encoded_text.assign(data.begin()+first,data.end());
        auto next=record.next;if(old && next && next!=UINT32_MAX){if(next>UINT32_MAX-position)bad();next+=position;}
        previous=position;position=next;records.push_back(std::move(record));
    }
    return records;
}
PhraseTable phrase_table(const File& file){
    constexpr std::size_t maximum=256U*1024U*1024U;
    PhraseTable result;
    if(const auto found=file.streams.find("|Phrases");found!=file.streams.end()){
        result.encoding=PhraseEncoding::classic;const Bytes b{found->second};
        const auto count=b.u16(0);if(b.u16(2)!=0x100)bad();
        const bool old=file.system.version<=16;const std::size_t base=old?4:8,table_size=2*(std::size_t(count)+1);
        (void)b.part(base,table_size);if(b.u16(base)!=table_size)bad();
        const auto encoded=b.part(base+table_size,b.data.size()-base-table_size);
        const auto data=old?std::vector<std::uint8_t>(encoded.begin(),encoded.end()):decompress(encoded,b.u32(4));
        if(!old && data.size()!=b.u32(4))bad();
        for(std::size_t i=0;i<count;++i){const auto first=b.u16(base+2*i),last=b.u16(base+2*i+2);if(first<table_size || last<first || last-table_size>data.size())bad();result.entries.emplace_back(data.begin()+static_cast<std::ptrdiff_t>(first-table_size),data.begin()+static_cast<std::ptrdiff_t>(last-table_size));}
        if(b.u16(base+2*count)-table_size!=data.size())bad();
        return result;
    }
    const auto index=file.streams.find("|PhrIndex"),image=file.streams.find("|PhrImage");
    if(index==file.streams.end() && image==file.streams.end())return result;
    if(index==file.streams.end() || image==file.streams.end())bad();
    const Bytes b{index->second};if(b.u32(0)!=1 || b.u32(20)!=0)bad();
    const auto count=b.u32(4),size=b.u32(12),packed=b.u32(16);const unsigned bits=b.u16(24)&15U;
    if(count>65536 || size>maximum || packed!=image->second.size() || bits>5)bad();
    (void)b.part(0,28);const auto data=packed==size?image->second:decompress(image->second,size);if(data.size()!=size)bad();
    std::size_t bit=0,offset=0;
    const auto take=[&](){const auto byte=28+bit/8;if(byte>=b.data.size())bad();const bool value=(b.data[byte]&(1U<<(bit%8)))!=0;++bit;return value;};
    result.encoding=PhraseEncoding::hall;
    for(std::uint32_t i=0;i<count;++i){std::size_t length=1;
        while(take()){if(length>size || (1U<<bits)>size-length)bad();length+=1U<<bits;}
        // 格式的最低位即使 BitCount 為零仍存在。
        for(unsigned j=0;j<std::max(bits,1U);++j)if(take())length+=1U<<j;
        if(offset>data.size() || length>data.size()-offset)bad();
        result.entries.emplace_back(data.begin()+static_cast<std::ptrdiff_t>(offset),data.begin()+static_cast<std::ptrdiff_t>(offset+length));offset+=length;
    }
    if(offset!=data.size())bad();
    return result;
}
std::vector<std::uint8_t> topic_text(const PhraseTable& phrases,const TopicRecord& record){
    const auto& input=record.encoded_text;const std::size_t size=record.text_size;if(size>256U*1024U*1024U)bad();
    if(size<=input.size())return {input.begin(),input.begin()+static_cast<std::ptrdiff_t>(size)};
    if(phrases.encoding==PhraseEncoding::none)bad();
    std::vector<std::uint8_t> result;std::size_t at=0;
    const auto byte=[&](){if(at>=input.size())bad();return input[at++];};
    const auto emit=[&](std::uint8_t value,std::size_t count){if(count>size-result.size())bad();result.insert(result.end(),count,value);};
    const auto phrase=[&](std::size_t number){if(number>=phrases.entries.size())bad();const auto& value=phrases.entries[number];if(value.size()>size-result.size())bad();result.insert(result.end(),value.begin(),value.end());};
    while(at<input.size()){
        const unsigned ch=byte();
        if(phrases.encoding==PhraseEncoding::classic){
            if(!ch || ch>15)emit(static_cast<std::uint8_t>(ch),1);
            else{const unsigned code=(ch-1)*256+byte();phrase(code/2);if(code&1)emit(' ',1);}
        }else if(!(ch&1))phrase(ch/2);
        else if((ch&3)==1)phrase(ch*64+64+byte());
        else if((ch&7)==3){const auto count=ch/8+1;if(count>input.size()-at || count>size-result.size())bad();result.insert(result.end(),input.begin()+static_cast<std::ptrdiff_t>(at),input.begin()+static_cast<std::ptrdiff_t>(at+count));at+=count;}
        else emit((ch&15)==7?' ':0,ch/16+1);
    }
    if(result.size()!=size)bad();return result;
}

std::uint32_t context_hash(std::string_view name){
    if(name.empty() || !name.front())return 1;std::uint32_t hash=0;
    for(const char raw:name){const auto ch=static_cast<unsigned char>(raw);if(!ch)break;
        int value=ch>=96 && ch<=127?int(ch)-80:int(ch)-48;
        if(ch==33 || ch==91 || ch==180)value=11;
        else if(ch==46 || ch==92)value=12;
        else if(ch==93 || ch==95)value=13;
        else if(ch==94)value=14;
        else if(ch==48)value=10;
        if(value>127)value-=256;
        hash=hash*43+static_cast<std::uint32_t>(value);
    }return hash;
}
TopicIndex topic_index(const File& file){
    TopicIndex result;
    if(const auto found=file.streams.find("|CTXOMAP");found!=file.streams.end()){
        const Bytes b{found->second};const auto count=b.u16(0);if(b.data.size()!=2+8*std::size_t(count))bad();
        for(std::size_t i=0;i<count;++i)if(!result.context_ids.emplace(b.u32(2+8*i),b.u32(6+8*i)).second)bad();
    }
    const auto tree=[&](const char* name,bool titles){
        const auto found=file.streams.find(name);if(found==file.streams.end())return;
        const Bytes b{found->second};if(b.u16(0)!=0x293b || !(b.u16(2)&2) || b.u16(22)!=0 || b.u16(28)!=0xffff)bad();
        const auto page_size=b.u16(4),pages=b.u16(30),levels=b.u16(32);const auto count=b.u32(34);
        if(page_size<8 || !pages || !levels || levels>32 || count>100000)bad();
        if(b.data[6]!='L' || b.data[7]!=(titles?'z':'4') || b.data[8]!=0)bad();
        (void)b.part(38,std::size_t(pages)*page_size);std::set<unsigned> visited;
        std::vector<unsigned> leaves;std::vector<std::pair<unsigned,unsigned>> links;std::size_t entries=0;
        const auto visit=[&](const auto& self,unsigned number,unsigned depth)->void{
            if(number>=pages || !visited.insert(number).second)bad();const Bytes page{b.part(38+std::size_t(number)*page_size,page_size)};
            const unsigned header=depth==1?8:6,unused=page.u16(0),n=page.u16(2);if(unused>page_size-header)bad();const Bytes used{page.data.first(page_size-unused)};std::size_t at=header;
            if(depth>1){self(self,used.u16(4),depth-1);for(unsigned j=0;j<n;++j){(void)used.u32(at);self(self,used.u16(at+4),depth-1);at+=6;}}
            else{leaves.push_back(number);links.emplace_back(page.u16(4),page.u16(6));
                for(unsigned j=0;j<n;++j){const auto key=used.u32(at);at+=4;
                    if(titles){auto title=used.string(at);if(!result.titles.emplace(key,std::move(title)).second)bad();}
                    else{const auto target=used.u32(at);at+=4;if(!result.context_hashes.emplace(key,target).second)bad();}
                    if(++entries>count)bad();
                }
            }
            if(at!=used.data.size())bad();
        };
        visit(visit,b.u16(26),levels);if(entries!=count)bad();
        for(std::size_t j=0;j<leaves.size();++j)if(links[j].first!=(j?leaves[j-1]:0xffffU) || links[j].second!=(j+1<leaves.size()?leaves[j+1]:0xffffU))bad();
    };
    tree("|CONTEXT",false);tree("|TTLBTREE",true);return result;
}

std::vector<TopicAddress> topic_addresses(const File& file,std::span<const TopicRecord> records){
    // HC30 使用 topic number，不能套用 HC31 的 TOPICOFFSET 規則。
    if(file.system.version<=16)bad();
    std::vector<TopicAddress> result;std::size_t header=records.size();std::uint32_t previous=0,page=UINT32_MAX,count=0;
    for(std::size_t i=0;i<records.size();++i){const auto& record=records[i];if(record.position<12 || record.position<=previous)bad();previous=record.position;
        const auto current=(record.position-12)/16384;if(current>0x1ffffU)bad();if(current!=page){page=current;count=0;}
        std::uint32_t length=0;
        if(record.kind==2){if(record.formatting.size()<28)bad();header=i;}
        else if(record.kind==0x20 || record.kind==0x23){
            const Bytes b{record.formatting};const auto first=b.u16(0);std::size_t at=2;
            if(first&1){(void)b.u16(at);at+=2;}
            const auto low=b.part(at,1)[0];++at;length=low/2;if(low&1)length+=128U*b.part(at,1)[0];
        }else bad();
        if(header==records.size() || count>32767 || length>32768-count)bad();
        result.push_back({page*32768+count,length,i,header});count+=length;
    }return result;
}
std::optional<TopicLocation> locate_topic(std::span<const TopicAddress> addresses,std::uint32_t offset){
    // 主題標頭不佔字元；同位置的正文仍屬於該標頭。
    for(const auto& address:addresses)if(address.offset==offset && address.record==address.header)return TopicLocation{address.record,address.header,0};
    for(const auto& address:addresses)if(offset>=address.offset && offset-address.offset<address.length)return TopicLocation{address.record,address.header,offset-address.offset};
    return std::nullopt;
}
TopicHeader topic_header(const PhraseTable& phrases,const TopicRecord& record){
    if(record.kind!=2 || record.formatting.size()!=28)bad();const Bytes b{record.formatting};
    TopicHeader result;result.browse_previous=b.u32(4);result.browse_next=b.u32(8);result.number=b.u32(12);result.non_scroll=b.u32(16);result.scroll=b.u32(20);result.next=b.u32(24);
    const auto data=topic_text(phrases,record);std::size_t at=0;
    // 最後一個字串可由記錄長度界定，Halibut 會省略最後的 NUL。
    const auto string=[&](){const auto first=at;while(at<data.size() && data[at])++at;std::string value(data.begin()+static_cast<std::ptrdiff_t>(first),data.begin()+static_cast<std::ptrdiff_t>(at));if(at<data.size())++at;return value;};
    result.title=string();while(at<data.size())result.macros.push_back(string());return result;
}

KeywordIndex keyword_index(const File& file,std::string_view prefix){
    if(prefix.empty() || prefix.size()>2 || (prefix.size()==2 && prefix[1]!='K'))bad();
    for(const char ch:prefix)if(!((ch>='A' && ch<='Z') || (ch>='a' && ch<='z') || (ch>='0' && ch<='9')))bad();
    const auto base="|"+std::string(prefix);const auto found=file.streams.find(base+"WBTREE"),data=file.streams.find(base+"WDATA");KeywordIndex result;
    if(found==file.streams.end() && data==file.streams.end())return result;
    if(found==file.streams.end() || data==file.streams.end())bad();const Bytes b{found->second},targets{data->second};
    if(targets.data.size()%4 || b.u16(0)!=0x293b || !(b.u16(2)&2) || b.u16(22)!=0 || b.u16(28)!=0xffff)bad();
    const auto page_size=b.u16(4),pages=b.u16(30),levels=b.u16(32);const auto count=b.u32(34);
    if(page_size<8 || !pages || !levels || levels>32 || count>100000)bad();
    if((b.data[6]!='F' && b.data[6]!='i' && b.data[6]!='z') || b.data[7]!='2' || b.data[8]!='4' || b.data[9]!=0)bad();
    (void)b.part(38,std::size_t(pages)*page_size);std::set<unsigned> visited;
    std::vector<unsigned> leaves;std::vector<std::pair<unsigned,unsigned>> links;std::size_t entries=0,references=0;
    const auto visit=[&](const auto& self,unsigned number,unsigned depth)->void{
        if(number>=pages || !visited.insert(number).second)bad();const Bytes page{b.part(38+std::size_t(number)*page_size,page_size)};
        const unsigned header=depth==1?8:6,unused=page.u16(0),n=page.u16(2);if(unused>page_size-header)bad();const Bytes used{page.data.first(page_size-unused)};std::size_t at=header;
        if(depth>1){self(self,used.u16(4),depth-1);for(unsigned j=0;j<n;++j){(void)used.string(at);self(self,used.u16(at),depth-1);at+=2;}}
        else{leaves.push_back(number);links.emplace_back(page.u16(4),page.u16(6));
            for(unsigned j=0;j<n;++j){auto keyword=used.string(at);const auto number_of_targets=used.u16(at);const auto offset=used.u32(at+2);at+=6;
                if(offset%4 || number_of_targets>1000000-references)bad();references+=number_of_targets;
                (void)targets.part(offset,4*std::size_t(number_of_targets));std::vector<std::uint32_t> values;
                for(unsigned k=0;k<number_of_targets;++k)values.push_back(targets.u32(std::size_t(offset)+4*k));
                if(!result.emplace(std::move(keyword),std::move(values)).second || ++entries>count)bad();
            }
        }
        if(at!=used.data.size())bad();
    };
    visit(visit,b.u16(26),levels);if(entries!=count)bad();
    for(std::size_t j=0;j<leaves.size();++j)if(links[j].first!=(j?leaves[j-1]:0xffffU) || links[j].second!=(j+1<leaves.size()?leaves[j+1]:0xffffU))bad();
    return result;
}

std::vector<Font> fonts(const File& file){
    const auto found=file.streams.find("|FONT");if(found==file.streams.end())return {};const Bytes b{found->second};
    const auto names=b.u16(0),count=b.u16(2),names_at=b.u16(4),descriptors_at=b.u16(6);
    // MVB 的 style／字元映射結構另有格式，不能誤用 classic 的 11-byte descriptor。
    if(names_at<8 || names_at>=12 || descriptors_at<names_at)bad();
    const auto name_bytes=unsigned(descriptors_at)-names_at;
    if((!names && (count || name_bytes)) || (names && (name_bytes%names || !name_bytes)))bad();
    (void)b.part(names_at,name_bytes);(void)b.part(descriptors_at,11*std::size_t(count));
    std::vector<std::string> faces;const auto width=names?name_bytes/names:0;
    for(unsigned i=0;i<names;++i){const auto bytes=b.part(std::size_t(names_at)+i*width,width);const auto end=std::find(bytes.begin(),bytes.end(),0);faces.emplace_back(bytes.begin(),end);}
    std::vector<Font> result;
    for(unsigned i=0;i<count;++i){const Bytes d{b.part(std::size_t(descriptors_at)+11*i,11)};const auto face=d.u16(3);if(face>=faces.size())bad();Font font;
        font.face=faces[face];font.attributes=d.data[0];font.height_twips=static_cast<std::uint16_t>(unsigned(d.data[1])*10);font.family=d.data[2];font.weight=(font.attributes&1)?700:400;
        font.foreground=std::uint32_t(d.data[5])|std::uint32_t(d.data[6])<<8|std::uint32_t(d.data[7])<<16;
        font.background=std::uint32_t(d.data[8])|std::uint32_t(d.data[9])<<8|std::uint32_t(d.data[10])<<16;result.push_back(std::move(font));
    }return result;
}

TextLayout text_layout(const PhraseTable& phrases,const TopicRecord& record){
    if(record.kind!=0x20 && record.kind!=0x23)bad();const Bytes b{record.formatting};std::size_t at=0;
    const auto byte=[&](){const auto value=b.part(at,1)[0];++at;return value;};
    const auto word=[&](){const auto value=b.u16(at);at+=2;return value;};
    const auto dword=[&](){const auto value=b.u32(at);at+=4;return value;};
    const auto ushort=[&](){const unsigned low=byte();return low/2+((low&1)?128U*byte():0U);};
    const auto sshort=[&](){const int low=byte();return low/2+((low&1)?128*int(byte())-16384:-64);};
    const auto slong=[&](){const std::int64_t low=word();return low/2+((low&1)?32768LL*word()-67108864LL:-16384LL);};
    const auto size=slong();const auto length=ushort();if(size<0 || static_cast<std::uint64_t>(size)>b.data.size()-at)bad();
    TextLayout layout;layout.table=record.kind==0x23;
    if(layout.table){const auto columns=byte();layout.table_type=byte();if(!columns || layout.table_type>3)bad();if(!(layout.table_type&1))layout.minimum_width=static_cast<std::int16_t>(word());
        for(unsigned i=0;i<columns;++i){const auto gap=static_cast<std::int16_t>(word());const auto width=static_cast<std::int16_t>(word());layout.columns.emplace_back(gap,width);}}
    const auto data=topic_text(phrases,record);const Bytes text{data};std::size_t pos=0,total_runs=0;
    do{
    Paragraph result;
    if(layout.table){result.column=static_cast<std::int16_t>(word());if(result.column==-1)break;if(result.column<0 || static_cast<std::size_t>(result.column)>=layout.columns.size())bad();(void)word();(void)byte();}
    (void)byte();(void)byte();result.id=word();result.flags=word();
    if(result.flags&1)(void)slong();
    for(unsigned i=0;i<6;++i)if(result.flags&(2U<<i))result.metrics[i]=sshort();
    if(result.flags&0x100){result.border=byte();result.border_width=static_cast<std::int16_t>(word());}
    if(result.flags&0x200){const auto count=sshort();if(count<0 || count>16383)bad();for(int i=0;i<count;++i){const auto position=ushort();const auto kind=(position&0x4000)?ushort():0;result.tabs.emplace_back(static_cast<std::uint16_t>(position&0x3fff),static_cast<std::uint16_t>(kind));}}
    while(true){if(++total_runs>1000000)bad();const auto start=pos;auto value=text.string(pos);if(!value.empty())result.runs.push_back({0,0,static_cast<std::uint32_t>(start),std::move(value)});
        const auto opcode=byte();if(opcode==0xff)break;TextRun run;run.opcode=opcode;run.character=static_cast<std::uint32_t>(pos);
        switch(opcode){
        case 0x80:case 0x21:run.argument=word();break;
        case 0x20:case 0xe0:case 0xe1:case 0xe2:case 0xe3:case 0xe6:case 0xe7:run.argument=dword();break;
        case 0x81:case 0x82:case 0x83:case 0x89:case 0x8b:case 0x8c:break;
        case 0xc8:case 0xcc:{const auto bytes=word();if(bytes<3)bad();const auto macro=b.part(at,bytes-3);at+=bytes-3;run.text.assign(macro.begin(),macro.end());break;}
        case 0xea:case 0xeb:case 0xee:case 0xef:{
            const auto bytes=word();const Bytes target{b.part(at,bytes)};at+=bytes;LinkTarget link;link.type=target.part(0,1)[0];run.argument=target.u32(1);std::size_t end=5;
            if(link.type==1){link.window_number=target.part(end,1)[0];++end;}
            else if(link.type==4 || link.type==6){link.file=target.string(end);if(link.type==6)link.window=target.string(end);}
            else if(link.type!=0)bad();
            if(end!=target.data.size())bad();run.link=std::move(link);break;
        }
        default:bad();
        }
        result.runs.push_back(std::move(run));
    }
    layout.paragraphs.push_back(std::move(result));if(layout.paragraphs.size()>100000)bad();
    }while(layout.table);
    if(at!=b.data.size() || pos!=data.size() || length!=data.size())bad();return layout;
}
Paragraph paragraph(const PhraseTable& phrases,const TopicRecord& record){
    if(record.kind!=0x20)bad();auto layout=text_layout(phrases,record);return std::move(layout.paragraphs.front());
}

}
