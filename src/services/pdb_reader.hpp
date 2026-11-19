#pragma once
// PDB 7.0（MSF 7.00）讀取：DbgHelp 的 PDB 符號與行號來源。只讀檔案內容，不執行或映射任何程式碼。
// - MSF：超級區塊（區塊大小、目錄大小、目錄區塊表位址）→ 串流目錄（串流數、各串流大小、區塊編號）。
// - 串流 1（PDB 資訊）：版本、簽章、Age、GUID 與具名串流表（找 /names 字串表）。
// - 串流 3（DBI）：模組資訊（模組符號串流、符號／C11／C13 位元組數、物件檔名）、選用偵錯標頭（區段標頭串流）。
// - 模組串流：S_GPROC32／S_LPROC32（含 _ID 版）的名稱、區段:位移與大小；C13 子區段 DEBUG_S_FILECHKSMS（檔名在 /names 的位移）
//   與 DEBUG_S_LINES（函式起點、程式碼長度、每個檔案區塊的位移與行號）。
// - 符號記錄串流：S_PUB32 公開符號。區段:位移以區段標頭的 VirtualAddress 轉成 RVA。
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace x64arm::pdb {
struct Line {std::uint32_t rva{},end{},line{},file{},module{};};
struct Symbol {std::string name;std::uint32_t rva{},size{};bool function{},global{},pub{};};
struct Database {
    std::array<std::uint8_t,16> guid{};std::uint32_t age{},signature{};
    std::vector<std::string> files;     // 依檔名表位移去重後的原始檔路徑
    std::vector<std::string> objects;   // 模組（物件檔）名稱
    std::vector<Line> lines;            // 依 RVA 排序
    std::vector<Symbol> symbols;        // 程序與公開符號
    std::vector<std::uint8_t> source_server; // 具名串流 srcsrv（沒有時為空）
};
struct CodeView {std::array<std::uint8_t,16> guid{};std::uint32_t age{};std::string path;};
namespace detail {
struct Failure {};
inline std::uint64_t read(std::span<const std::uint8_t> bytes,std::size_t at,unsigned size){
    if(at>bytes.size() || size>bytes.size()-at)throw Failure{};
    std::uint64_t value=0;for(unsigned i=0;i<size;++i)value|=std::uint64_t{bytes[at+i]}<<(8*i);return value;
}
inline std::string cstring(std::span<const std::uint8_t> bytes,std::size_t at,std::size_t limit=4096){
    std::string out;while(true){if(at>=bytes.size())throw Failure{};const auto c=bytes[at++];if(!c)break;out.push_back(static_cast<char>(c));if(out.size()>limit)throw Failure{};}
    return out;
}
}
// PE 映像的 CodeView（RSDS）偵錯目錄：GUID、Age 與 PDB 路徑。
inline std::optional<CodeView> image_codeview(std::span<const std::uint8_t> bytes){
    using detail::read;
    try{
        if(read(bytes,0,2)!=0x5a4d)return std::nullopt;const auto pe=static_cast<std::size_t>(read(bytes,60,4));if(read(bytes,pe,4)!=0x4550)return std::nullopt;
        const auto sections=read(bytes,pe+6,2),optional_size=read(bytes,pe+20,2);const auto optional=pe+24;const auto magic=read(bytes,optional,2);
        const std::size_t directories=optional+(magic==0x20b?112:96);if(read(bytes,optional+(magic==0x20b?108:92),4)<7)return std::nullopt;
        const auto rva=read(bytes,directories+6*8,4),size=read(bytes,directories+6*8+4,4);if(!rva || !size)return std::nullopt;
        const auto table=optional+static_cast<std::size_t>(optional_size);
        const auto file_offset=[&](std::uint64_t address)->std::optional<std::size_t>{
            for(std::uint64_t i=0;i<sections;++i){const auto at=table+static_cast<std::size_t>(i)*40;const auto va=read(bytes,at+12,4),extent=std::max(read(bytes,at+8,4),read(bytes,at+16,4)),raw=read(bytes,at+20,4);
                if(address>=va && address<va+extent)return static_cast<std::size_t>(raw+(address-va));}
            return std::nullopt;};
        const auto directory=file_offset(rva);if(!directory)return std::nullopt;
        for(std::uint64_t i=0;i<size/28;++i){
            const auto entry=*directory+static_cast<std::size_t>(i)*28;if(read(bytes,entry+12,4)!=2)continue; // IMAGE_DEBUG_TYPE_CODEVIEW
            const auto length=read(bytes,entry+16,4),pointer=read(bytes,entry+24,4);if(length<24 || read(bytes,static_cast<std::size_t>(pointer),4)!=0x53445352U)continue;
            CodeView view;for(unsigned n=0;n<16;++n)view.guid[n]=static_cast<std::uint8_t>(read(bytes,static_cast<std::size_t>(pointer)+4+n,1));
            view.age=static_cast<std::uint32_t>(read(bytes,static_cast<std::size_t>(pointer)+20,4));
            view.path=detail::cstring(bytes.first(std::min<std::size_t>(bytes.size(),static_cast<std::size_t>(pointer+length))),static_cast<std::size_t>(pointer)+24,1024);
            return view;
        }
    }catch(const detail::Failure&){}
    return std::nullopt;
}
inline std::optional<Database> parse(std::span<const std::uint8_t> bytes){
    using detail::read;using detail::Failure;
    static constexpr char magic[]="Microsoft C/C++ MSF 7.00\r\n\x1a""DS\0\0";
    if(bytes.size()<56 || std::memcmp(bytes.data(),magic,sizeof magic-1))return std::nullopt;
    try{
        const auto block=static_cast<std::size_t>(read(bytes,32,4)),directory_bytes=static_cast<std::size_t>(read(bytes,44,4)),map=static_cast<std::size_t>(read(bytes,52,4));
        if(block!=512 && block!=1024 && block!=2048 && block!=4096)return std::nullopt;
        const auto block_at=[&](std::uint64_t index)->std::span<const std::uint8_t>{const auto at=static_cast<std::size_t>(index)*block;if(at>bytes.size() || block>bytes.size()-at)throw Failure{};return bytes.subspan(at,block);};
        const auto gather=[&](std::span<const std::uint8_t> list,std::size_t size){
            std::vector<std::uint8_t> out;if(size>256U*1024U*1024U)throw Failure{};out.reserve(size);
            for(std::size_t i=0;out.size()<size;++i){const auto part=block_at(read(list,i*4,4));out.insert(out.end(),part.begin(),part.begin()+static_cast<std::ptrdiff_t>(std::min(block,size-out.size())));}
            return out;};
        const auto directory_blocks=(directory_bytes+block-1)/block;
        std::vector<std::uint8_t> list;for(std::size_t i=0;i<directory_blocks;++i){const auto value=read(block_at(map),i*4,4);for(unsigned n=0;n<4;++n)list.push_back(static_cast<std::uint8_t>(value>>(8*n)));}
        const auto directory=gather(list,directory_bytes);
        const auto count=static_cast<std::size_t>(read(directory,0,4));if(count>65536)throw Failure{};
        std::vector<std::vector<std::uint8_t>> streams(count);std::size_t cursor=4+count*4;
        for(std::size_t i=0;i<count;++i){
            const auto size=read(directory,4+i*4,4);if(size==0xffffffffU)continue;const auto blocks=(static_cast<std::size_t>(size)+block-1)/block;
            if(cursor+blocks*4>directory.size())throw Failure{};
            streams[i]=gather(std::span(directory).subspan(cursor,blocks*4),static_cast<std::size_t>(size));cursor+=blocks*4;
        }
        const auto stream=[&](std::uint64_t index)->const std::vector<std::uint8_t>&{static const std::vector<std::uint8_t> empty;return index<streams.size()?streams[static_cast<std::size_t>(index)]:empty;};
        Database db;
        // PDB 資訊串流與具名串流表。
        const auto& info=stream(1);if(info.size()<28)throw Failure{};
        db.signature=static_cast<std::uint32_t>(read(info,4,4));db.age=static_cast<std::uint32_t>(read(info,8,4));std::copy_n(info.begin()+12,16,db.guid.begin());
        std::optional<std::uint32_t> names_stream,srcsrv_stream;
        {
            const auto strings=static_cast<std::size_t>(read(info,28,4));const std::size_t text=32,table=text+strings;
            const auto size=read(info,table,4);(void)size;const auto capacity=static_cast<std::size_t>(read(info,table+4,4));
            const auto present_words=static_cast<std::size_t>(read(info,table+8,4));std::size_t at=table+12;std::vector<std::uint32_t> present(present_words);
            for(auto& word:present){word=static_cast<std::uint32_t>(read(info,at,4));at+=4;}
            const auto deleted_words=static_cast<std::size_t>(read(info,at,4));at+=4+deleted_words*4;
            for(std::size_t bucket=0;bucket<capacity && bucket/32<present.size();++bucket){
                if(!(present[bucket/32]&(1U<<(bucket%32))))continue;
                const auto key=static_cast<std::size_t>(read(info,at,4));const auto value=static_cast<std::uint32_t>(read(info,at+4,4));at+=8;
                if(key>=strings)throw Failure{};const auto name=detail::cstring(info,text+key,256);
                if(name=="/names")names_stream=value;else if(name=="srcsrv")srcsrv_stream=value;
            }
        }
        if(srcsrv_stream)db.source_server=stream(*srcsrv_stream);
        const auto& names=names_stream?stream(*names_stream):stream(UINT32_MAX);
        const auto name_at=[&](std::uint64_t offset)->std::string{
            if(names.size()<12 || read(names,0,4)!=0xeffeeffeU)throw Failure{};const auto length=read(names,8,4);if(offset>=length)throw Failure{};
            return detail::cstring(std::span(names).first(std::min<std::size_t>(names.size(),12+static_cast<std::size_t>(length))),12+static_cast<std::size_t>(offset),1024);};
        // DBI：模組清單、符號記錄串流、區段標頭。
        const auto& dbi=stream(3);if(dbi.size()<64)throw Failure{};
        db.age=static_cast<std::uint32_t>(read(dbi,8,4));
        const auto records=read(dbi,20,2);const auto module_size=static_cast<std::size_t>(read(dbi,24,4)),contribution=static_cast<std::size_t>(read(dbi,28,4)),
            section_map=static_cast<std::size_t>(read(dbi,32,4)),source_info=static_cast<std::size_t>(read(dbi,36,4)),type_server=static_cast<std::size_t>(read(dbi,40,4)),
            optional=static_cast<std::size_t>(read(dbi,48,4)),ec=static_cast<std::size_t>(read(dbi,52,4));
        const auto optional_at=64+module_size+contribution+section_map+source_info+type_server+ec;
        std::vector<std::uint32_t> section_rva;
        if(optional>=12){const auto index=read(dbi,optional_at+10,2);if(index!=0xffff){const auto& headers=stream(index);for(std::size_t at=0;at+40<=headers.size();at+=40)section_rva.push_back(static_cast<std::uint32_t>(read(headers,at+12,4)));}}
        const auto rva_of=[&](std::uint64_t segment,std::uint64_t offset)->std::optional<std::uint32_t>{
            if(!segment || segment>section_rva.size())return std::nullopt;return static_cast<std::uint32_t>(section_rva[static_cast<std::size_t>(segment-1)]+offset);};
        std::map<std::uint32_t,std::uint32_t> file_index; // /names 位移 → files 索引
        for(std::size_t at=64;at<64+module_size;){
            const auto symbols_stream=read(dbi,at+34,2);const auto symbol_bytes=static_cast<std::size_t>(read(dbi,at+36,4)),c11=static_cast<std::size_t>(read(dbi,at+40,4)),c13=static_cast<std::size_t>(read(dbi,at+44,4));
            std::size_t name=at+64;const auto module_name=detail::cstring(dbi,name);name+=module_name.size()+1;const auto object=detail::cstring(dbi,name);name+=object.size()+1;
            const auto module_index=static_cast<std::uint32_t>(db.objects.size());db.objects.push_back(object.empty()?module_name:object);
            at=(name+3)&~std::size_t{3};
            if(symbols_stream==0xffff)continue;
            const auto& module=stream(symbols_stream);if(symbol_bytes>module.size() || c11>module.size()-symbol_bytes || c13>module.size()-symbol_bytes-c11)throw Failure{};
            for(std::size_t record=4;record+4<=symbol_bytes;){
                const auto length=static_cast<std::size_t>(read(module,record,2));const auto kind=read(module,record+2,2);if(length<2)throw Failure{};
                if((kind==0x1110 || kind==0x110f || kind==0x1147 || kind==0x1146) && length>=37){
                    const auto size=read(module,record+16,4),offset=read(module,record+32,4),segment=read(module,record+36,2);
                    if(const auto rva=rva_of(segment,offset))db.symbols.push_back({detail::cstring(module,record+39),*rva,static_cast<std::uint32_t>(size),true,kind==0x1110 || kind==0x1147,false});
                }
                record+=length+2;
            }
            std::map<std::uint32_t,std::uint32_t> checksum_file; // FILECHKSMS 內位移 → files 索引
            const auto c13_at=symbol_bytes+c11;
            for(int pass=0;pass<2;++pass)for(std::size_t sub=c13_at;sub+8<=c13_at+c13;){
                const auto kind=read(module,sub,4);const auto length=static_cast<std::size_t>(read(module,sub+4,4));const auto data=sub+8;if(length>c13_at+c13-data)throw Failure{};
                if(pass==0 && kind==0xf4){
                    for(std::size_t entry=0;entry+6<=length;){
                        const auto offset=static_cast<std::uint32_t>(read(module,data+entry,4));const auto checksum=read(module,data+entry+4,1);
                        auto found=file_index.find(offset);if(found==file_index.end()){found=file_index.emplace(offset,static_cast<std::uint32_t>(db.files.size())).first;db.files.push_back(name_at(offset));}
                        checksum_file[static_cast<std::uint32_t>(entry)]=found->second;entry=(entry+6+static_cast<std::size_t>(checksum)+3)&~std::size_t{3};
                    }
                }else if(pass==1 && kind==0xf2 && length>=12){
                    const auto offset=read(module,data,4),segment=read(module,data+4,2),flags=read(module,data+6,2),code=read(module,data+8,4);
                    const auto base=rva_of(segment,offset);
                    for(std::size_t file=data+12;file+12<=data+length;){
                        const auto checksum=static_cast<std::uint32_t>(read(module,file,4));const auto entries=static_cast<std::size_t>(read(module,file+4,4));const auto size=static_cast<std::size_t>(read(module,file+8,4));
                        if(size<12 || size>data+length-file || entries>(size-12)/8)throw Failure{};
                        const auto found=checksum_file.find(checksum);
                        if(base && found!=checksum_file.end())for(std::size_t i=0;i<entries;++i){
                            const auto line_offset=read(module,file+12+i*8,4),bits=read(module,file+12+i*8+4,4);
                            const auto next=i+1<entries?read(module,file+12+(i+1)*8,4):code;
                            const auto number=static_cast<std::uint32_t>(bits&0xffffffU);if(!number || number==0xfeefee || number==0xf00f00)continue;
                            db.lines.push_back({static_cast<std::uint32_t>(*base+line_offset),static_cast<std::uint32_t>(*base+std::max(next,line_offset+1)),number,found->second,module_index});
                        }
                        (void)flags;file+=size;
                    }
                }
                sub=data+((length+3)&~std::size_t{3});
            }
        }
        // 公開符號：沒有對應程序符號的位址才加入（程序符號已帶大小）。
        const auto& publics=stream(records);
        for(std::size_t record=0;record+4<=publics.size();){
            const auto length=static_cast<std::size_t>(read(publics,record,2));const auto kind=read(publics,record+2,2);if(length<2)break;
            if(kind==0x110e && length>=12){
                const auto flags=read(publics,record+4,4),offset=read(publics,record+8,4),segment=read(publics,record+12,2);
                if(const auto rva=rva_of(segment,offset)){auto name=detail::cstring(publics,record+14);
                    if(std::none_of(db.symbols.begin(),db.symbols.end(),[&](const Symbol& item){return item.rva==*rva && item.name==name;}))db.symbols.push_back({std::move(name),*rva,0,(flags&2U)!=0,true,true});}
            }
            record+=length+2;
        }
        std::sort(db.lines.begin(),db.lines.end(),[](const Line& a,const Line& b){return a.rva<b.rva;});
        return db;
    }catch(const detail::Failure&){return std::nullopt;}catch(const std::bad_alloc&){return std::nullopt;}
}
}
