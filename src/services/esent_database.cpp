#include "x64arm/esent_database.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <functional>
#include <set>

namespace x64arm::esent {
namespace {
using Bytes=std::span<const std::uint8_t>;
[[noreturn]] void bad(){throw Error{Failure::Corrupt,"ESE 資料結構無效"};}
void need(bool value){if(!value)bad();}
Bytes slice(Bytes b,std::size_t at,std::size_t n){need(at<=b.size() && n<=b.size()-at);return b.subspan(at,n);}
std::uint64_t integer(Bytes b,std::size_t at,unsigned n){auto s=slice(b,at,n);std::uint64_t v=0;for(unsigned i=0;i<n;++i)v|=std::uint64_t(s[i])<<(8*i);return v;}
std::uint32_t u32(Bytes b,std::size_t at){return static_cast<std::uint32_t>(integer(b,at,4));}
std::uint16_t u16(Bytes b,std::size_t at){return static_cast<std::uint16_t>(integer(b,at,2));}
Binary copy(Bytes b){return {b.begin(),b.end()};}
std::string string(Bytes b){need(std::find(b.begin(),b.end(),0)==b.end());return {b.begin(),b.end()};}
std::string folded(std::string_view s){std::string out(s);for(auto& c:out)if(c>='A' && c<='Z')c+=32;return out;}
Bytes data(const Database& db,const Row& row){return slice(*db.bytes,row.offset,row.size);}
std::size_t fixed_end(Bytes record){need(record.size()>=4);const auto end=u16(record,2);need(end>=4+(record[0]+7U)/8U && end<=record.size());return end;}
std::optional<Bytes> variable(Bytes record,std::uint32_t id){
    const auto end=fixed_end(record);need(record[1]>=127);const auto count=static_cast<std::size_t>(record[1]-127);
    slice(record,end,count*2);if(id<128 || id>record[1])return {};
    const auto i=static_cast<std::size_t>(id-128);const auto stop=u16(record,end+i*2);const auto start=i?u16(record,end+(i-1)*2)&0x7fffU:0;
    need(start<=(stop&0x7fffU));auto part=slice(record,end+count*2+start,(stop&0x7fffU)-start);return stop&0x8000U?std::optional<Bytes>{}:part;
}
std::size_t tagged_start(Bytes record){const auto end=fixed_end(record);need(record[1]>=127);const auto count=static_cast<std::size_t>(record[1]-127);slice(record,end,count*2);const auto at=end+count*2+(count?(u16(record,end+(count-1)*2)&0x7fffU):0);need(at<=record.size());return at;}
struct Page {Bytes bytes;std::size_t base{},header{},count{},reserved{};std::uint32_t flags{},object{};bool large{};};
Page page(const Database& db,std::uint32_t number){
    need(number && std::uint64_t(number)+1<db.bytes->size()/db.page_size);Page p;
    p.base=(static_cast<std::size_t>(number)+1)*db.page_size;p.bytes=slice(*db.bytes,p.base,db.page_size);p.flags=u32(p.bytes,36);p.object=u32(p.bytes,24);
    p.large=db.page_size>8192;p.header=p.large?80:40;const auto tags=u16(p.bytes,34);p.count=tags&0xfffU;p.reserved=(tags>>12)&7U;if(!p.reserved)p.reserved=1;
    need(p.count>=p.reserved && p.count*4<=db.page_size-p.header);
    bool valid=false;
    if(p.flags&0x2000){
        const std::size_t block=p.large?(db.page_size<=16384?4096:8192):db.page_size;
        valid=integer(p.bytes,0,8)==block_checksum(p.bytes.first(block),number,true);
        if(p.large){need(integer(p.bytes,64,8)==number);for(std::size_t i=1,at=block;at<p.bytes.size();++i,at+=block){need(i<=3);valid=valid && integer(p.bytes,40+(i-1)*8,8)==block_checksum(p.bytes.subspan(at,std::min(block,p.bytes.size()-at)),number,false);}}
    }else valid=integer(p.bytes,0,8)==(std::uint64_t(number)<<32|header_checksum(p.bytes));
    if(!valid)throw Error{Failure::Checksum,"ESE 頁面校驗失敗"};return p;
}
struct Node {Bytes bytes;std::size_t offset{};unsigned flags{};};
Node node(const Page& p,std::size_t tag){
    need(tag<p.count);const auto at=p.bytes.size()-(tag+1)*4;const auto mask=p.large?0x7fffU:0x1fffU;
    const auto length=u16(p.bytes,at)&mask;const auto raw=u16(p.bytes,at+2);const auto offset=p.header+(raw&mask);
    need(offset<=p.bytes.size()-p.count*4 && length<=p.bytes.size()-p.count*4-offset);auto b=slice(p.bytes,offset,length);
    return {b,p.base+offset,p.large?(b.size()>=2?static_cast<unsigned>(b[1]>>5):0):static_cast<unsigned>(raw>>13)};
}
std::vector<Row> tree(const Database& db,std::uint32_t root){
    std::vector<Row> result;std::set<std::uint32_t> visited;const auto object=page(db,root).object;
    std::function<void(std::uint32_t,unsigned)> walk=[&](std::uint32_t number,unsigned depth){
        need(depth<128 && visited.insert(number).second);const auto p=page(db,number);need(p.object==object && !(p.flags&0x20));
        auto prefix=node(p,0).bytes;
        for(std::size_t i=p.reserved;i<p.count;++i){const auto n=node(p,i);if(n.flags&2)continue;need(n.bytes.size()>=2);
            std::size_t at=0,common=0;if(n.flags&4){common=u16(n.bytes,0)&0x1fffU;at=2;need(common<=prefix.size());}
            const auto size=u16(n.bytes,at)&0x1fffU;at+=2;auto suffix=slice(n.bytes,at,size);at+=size;
            if(p.flags&2){Binary key(prefix.begin(),prefix.begin()+static_cast<std::ptrdiff_t>(common));key.insert(key.end(),suffix.begin(),suffix.end());result.push_back({std::move(key),n.offset+at,n.bytes.size()-at});if(result.size()>1000000)throw Error{Failure::Limit,"ESE 資料列超過資源上限"};}
            else {need(n.bytes.size()-at==4);walk(u32(n.bytes,at),depth+1);}
        }
    };walk(root,0);return result;
}
Cell simple(Bytes b){return {false,{{copy(b),false,false}}};}
Cell fallback(const Column& c){return c.default_value?simple(*c.default_value):Cell{};}
Cell tagged(Bytes b,std::uint8_t flags){
    if(flags&0x20)return {};if(flags&0x40)throw Error{Failure::Unsupported,"ESE 加密欄位尚未支援"};need(!(flags&0x80));
    Cell out;out.is_null=false;
    if((flags&0x18)==0x18){need(!(flags&7) && !b.empty());const auto first=b[0];out.values={{copy(slice(b,1,first)),false,false},{copy(b.subspan(1+first)),false,false}};return out;}
    if(flags&8){need(!(flags&0x14));const auto count=(u16(b,0)&0x7fffU)/2;need(count && count*2<=b.size());for(unsigned i=0;i<count;++i){const auto start=u16(b,i*2);const auto at=start&0x7fffU;const auto end=i+1<count?(u16(b,(i+1)*2)&0x7fffU):b.size();need(at>=count*2 && at<=end);out.values.push_back({copy(slice(b,at,end-at)),bool(start&0x8000),bool(!i && (flags&2))});}return out;}
    need(!(flags&0x10) && !((flags&4) && (flags&2)));out.values.push_back({copy(b),bool(flags&4),bool(flags&2)});return out;
}
}
std::uint32_t header_checksum(std::span<const std::uint8_t> b){need(b.size()%4==0);std::uint32_t sum=0x89abcdef;for(std::size_t i=4;i<b.size();i+=4)sum^=u32(b,i);return sum;}
// Microsoft ESE 的 HSIAO ECC/XOR 算法；改為有界、未對齊安全的 C++20 讀取。
// Copyright (c) Microsoft Corporation. MIT；見 third_party/ese-reference-LICENSE。
std::uint64_t block_checksum(std::span<const std::uint8_t> b,std::uint32_t pg,bool header){
    need(b.size()>=1024 && b.size()<=8192 && std::has_single_bit(b.size()));std::array<std::uint32_t,4> cols{};std::uint32_t parity=0,index=0xff800000;
    const auto mask=[](std::uint32_t v){return 0U-static_cast<std::uint32_t>(std::popcount(v)&1);};
    for(std::size_t i=0;i<b.size();i+=16){std::uint32_t row=0;for(unsigned c=0;c<4;++c){const auto word=header && i+c*4<8?0:u32(b,i+c*4);cols[c]^=word;row^=word;}parity^=index&mask(row);index+=0xff800080;}
    parity|=0x00400000&mask(cols[0]^cols[1]);parity|=0x40&mask(cols[2]^cols[3]);const auto r0=cols[0]^cols[2],r1=cols[1]^cols[3],x=r0^r1;
    parity|=0x00200000&mask(r0);parity|=0x20&mask(r1);std::uint32_t r=0;index=0xffff0000;
    for(unsigned i=0;i<32;++i){if(x&(1U<<i))r^=index;index+=0xffff0001;}
    const auto size_mask=(static_cast<std::uint32_t>(b.size())<<19)-1;const auto ecc=(parity&0xffe0ffe0&size_mask)|(r&0x001f001f);return (std::uint64_t(ecc)<<32)|(x^pg);
}
const Table& table(const Database& db,std::string_view name){const auto key=folded(name);for(const auto& [n,t]:db.tables)if(folded(n)==key)return t;throw Error{Failure::MissingTable,"ESE 資料表不存在"};}
bool Database::operator==(const Database& other)const{return version==other.version && revision==other.revision && page_size==other.page_size && state==other.state && backup_header==other.backup_header && tables==other.tables && (bytes==other.bytes || (bytes && other.bytes && *bytes==*other.bytes));}
const Column& column(const Table& t,std::string_view name){const auto key=folded(name);for(const auto& [id,c]:t.columns){(void)id;if(folded(c.name)==key)return c;}throw Error{Failure::MissingColumn,"ESE 欄位不存在"};}
std::vector<Row> rows(const Database& db,const Table& t){return tree(db,t.root);}
Cell raw_cell(const Database& db,const Table& t,const Row& row,std::uint32_t id){
    const auto found=t.columns.find(id);if(found==t.columns.end())throw Error{Failure::MissingColumn,"ESE 欄位不存在"};const auto& c=found->second;const auto fid=id&0xffffU;const auto b=data(db,row);const auto end=fixed_end(b);
    if(fid<128){if(fid>b[0])return fallback(c);const auto bitmap=end-(b[0]+7U)/8U;if(b[bitmap+(fid-1)/8]&(1U<<((fid-1)%8)))return {};need(c.offset>=4 && c.offset+c.maximum<=bitmap);return simple(slice(b,c.offset,c.maximum));}
    if(fid<256){if(fid>b[1])return fallback(c);auto part=variable(b,fid);return part?simple(*part):Cell{};}
    const auto start=tagged_start(b);if(start==b.size())return fallback(c);auto fields=b.subspan(start);const auto offset_mask=db.page_size>8192?0x7fffU:0x1fffU;const auto count=(u16(fields,2)&offset_mask)/4;
    need(count && count*4<=fields.size() && (u16(fields,2)&offset_mask)%4==0);
    for(unsigned i=0;i<count;++i){const auto field=u16(fields,i*4);const auto position=u16(fields,i*4+2);const auto at=position&offset_mask;const auto stop=i+1<count?(u16(fields,(i+1)*4+2)&offset_mask):fields.size();need(at>=count*4 && at<=stop);auto part=slice(fields,at,stop-at);
        if(field!=fid || bool(position&0x8000)!=c.inherited)continue;
        if(db.page_size<=8192 && (position&0x2000))return {};
        if(db.page_size>8192 || (position&0x4000)){need(!part.empty());return tagged(part.subspan(1),part[0]);}return simple(part);
    }return fallback(c);
}
Database parse(Binary bytes,std::size_t limit){
    if(bytes.size()>limit)throw Error{Failure::Limit,"ESE 資料庫超過資源上限"};Database db;bool valid=false;
    for(const auto size:{2048U,4096U,8192U,16384U,32768U})for(unsigned backup=0;backup<2 && !valid;++backup){
        const std::size_t at=backup*size;if(bytes.size()<at+size)continue;auto h=Bytes(bytes).subspan(at,size);
        if(u32(h,4)!=0x89abcdef || u32(h,8)!=0x620 || u32(h,12)!=0 || (u32(h,236)?u32(h,236):4096)!=size || u32(h,0)!=header_checksum(h))continue;
        db.version=u32(h,8);db.revision=u32(h,232);db.page_size=size;db.state=u32(h,52);db.backup_header=backup!=0;valid=true;
    }
    if(!valid)throw Error{Failure::InvalidHeader,"ESE 主／備援標頭皆無效"};need(bytes.size()%db.page_size==0);db.bytes=std::make_shared<const Binary>(std::move(bytes));
    const auto catalog=tree(db,4);std::map<std::uint32_t,std::string> names;
    for(const auto& row:catalog){auto b=data(db,row);fixed_end(b);need(b[0]>=6);if(u16(b,8)!=1)continue;auto name=variable(b,128);need(name.has_value());Table t;t.object=u32(b,4);t.root=u32(b,14);t.flags=u32(b,22);t.name=string(*name);auto parent=variable(b,130);if(parent)t.template_name=string(*parent);if(b[0]>=12)t.chunk_max=u32(b,39);need(!t.name.empty() && !db.tables.contains(t.name) && !names.contains(t.object));names[t.object]=t.name;db.tables.emplace(t.name,std::move(t));}
    for(const auto& row:catalog){auto b=data(db,row);const auto object=u32(b,4);const auto type=u16(b,8);if(type==1)continue;const auto name=names.find(object);need(name!=names.end());auto& t=db.tables.at(name->second);
        if(type==2){auto n=variable(b,128);need(n.has_value() && b[0]>=7);Column c;c.id=u32(b,10);c.type=u32(b,14);c.maximum=u32(b,18);c.flags=u32(b,22);c.codepage=u32(b,26);c.name=string(*n);if(b[0]>=9)c.offset=u16(b,31);auto def=variable(b,131);if(def)c.default_value=copy(*def);need(c.id && c.id<=0xffff && !t.columns.contains(c.id));t.columns.emplace(c.id,std::move(c));}
        else if(type==4){need(!t.long_root);t.long_root=u32(b,14);}
        else need(type==3 || type==5);
    }
    std::set<std::string> done,active;
    std::function<void(Table&)> inherit=[&](Table& t){if(done.contains(t.name))return;need(active.insert(t.name).second);if(!t.template_name.empty()){auto it=db.tables.find(t.template_name);need(it!=db.tables.end());inherit(it->second);for(const auto& [id,c]:it->second.columns){auto inherited=c;inherited.inherited=true;need(!t.columns.contains(id));t.columns.emplace(id,std::move(inherited));}}active.erase(t.name);done.insert(t.name);};
    for(auto& [name,t]:db.tables){(void)name;inherit(t);}return db;
}
Binary decompress(std::span<const std::uint8_t> b,std::size_t limit){
    need(!b.empty());const auto scheme=b[0]>>3;Binary out;
    if(scheme==1 || scheme==2){need(b.size()>=2);const auto bits=(b.size()-2)*8+(b[0]&7U)+1;need(bits%7==0);const auto count=bits/7;if(count>limit/(scheme==2?2:1))throw Error{Failure::Limit,"ESE 解壓縮超過資源上限"};for(std::size_t i=0;i<count;++i){const auto pos=i*7,at=1+pos/8;std::uint32_t word=b[at];if(at+1<b.size())word|=std::uint32_t(b[at+1])<<8;out.push_back(static_cast<std::uint8_t>((word>>(pos%8))&127));if(scheme==2)out.push_back(0);}return out;}
    if(scheme!=3)throw Error{Failure::Unsupported,"ESE 壓縮種類尚未支援"};
    const auto expected=u16(b,1);if(expected>limit)throw Error{Failure::Limit,"ESE 解壓縮超過資源上限"};std::size_t at=3,nibble=0;std::uint32_t flags=0;unsigned bit=0;
    while(out.size()<expected){if(!bit){flags=u32(b,at);at+=4;bit=32;}if(!(flags&(1U<<--bit))){out.push_back(static_cast<std::uint8_t>(integer(b,at++,1)));continue;}
        const auto token=u16(b,at);at+=2;const auto distance=static_cast<std::size_t>((token>>3)+1);std::size_t length=token&7U;
        if(length==7){if(!nibble){nibble=at;length=integer(b,at++,1)&15U;}else {length=integer(b,nibble,1)>>4;nibble=0;}if(length==15){length=integer(b,at++,1);if(length==255){length=u16(b,at);at+=2;if(!length){length=u32(b,at);at+=4;}need(length>=22);length-=22;}length+=15;}length+=7;}length+=3;
        need(distance<=out.size() && length<=expected-out.size());for(std::size_t i=0;i<length;++i)out.push_back(out[out.size()-distance]);
    }return out;
}
Binary value(const Database& db,const Table& t,const Value& v,std::size_t limit){
    if(!v.separated){if(v.compressed)return decompress(v.bytes,limit);if(v.bytes.size()>limit)throw Error{Failure::Limit,"ESE 欄位超過資源上限"};return v.bytes;}
    need(t.long_root && (v.bytes.size()==4 || v.bytes.size()==8));Binary key(v.bytes.rbegin(),v.bytes.rend());auto entries=tree(db,t.long_root);std::optional<std::uint32_t> length;std::map<std::uint32_t,Bytes> chunks;
    for(const auto& row:entries){if(row.key==key){auto root=data(db,row);need(root.size()>=8);length=u32(root,4);}else if(row.key.size()==key.size()+4 && std::equal(key.begin(),key.end(),row.key.begin())){std::uint32_t offset=0;for(std::size_t i=key.size();i<row.key.size();++i)offset=(offset<<8)|row.key[i];need(chunks.emplace(offset,data(db,row)).second);}}
    need(length.has_value());if(*length>limit)throw Error{Failure::Limit,"ESE 長欄位超過資源上限"};Binary out;out.reserve(*length);
    for(auto it=chunks.begin();it!=chunks.end();++it){need(it->first==out.size());auto next=std::next(it);const auto end=next==chunks.end()?*length:next->first;need(end>=it->first && end<=*length);const auto expected=end-it->first;auto part=it->second.size()==expected?copy(it->second):decompress(it->second,expected);need(part.size()==expected);out.insert(out.end(),part.begin(),part.end());}need(out.size()==*length);return out;
}
}
