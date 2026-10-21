#include "x64arm/windows_services.hpp"
#include <algorithm>
#include <set>

namespace x64arm {
// fontsub CreateFontPackage：TrueType（glyf 輪廓）子集化。保留指定字形、glyph 0 及複合字形引用的元件，
// 字形索引不變（未保留者為空輪廓），loca 改為長格式並重算表格與 head 檢查碼；其餘表格原樣保留。
// 與 Windows 相同，只支援 TrueType 輪廓；CFF 字型回報缺少 glyf。
namespace {
constexpr std::uint32_t err_generic=1000,err_read_bounds=1001,err_mem=1005,err_format=1006,err_invalid_delta=1013,err_not_ttc=1014,err_ttc_index=1015;
constexpr std::uint32_t err_missing_cmap=1030,err_missing_glyf=1031,err_missing_head=1032,err_missing_loca=1035,err_missing_maxp=1036,err_parameter=1100;
struct Failure {std::uint32_t code;};
std::uint32_t be(std::span<const std::uint8_t> data,std::size_t at,unsigned width){
    if(at>data.size() || width>data.size()-at)throw Failure{err_read_bounds};
    std::uint32_t value=0;for(unsigned i=0;i<width;++i)value=(value<<8)|data[at+i];return value;
}
void put(std::vector<std::uint8_t>& out,std::size_t at,std::uint32_t value,unsigned width){for(unsigned i=0;i<width;++i)out[at+i]=static_cast<std::uint8_t>(value>>(8*(width-1-i)));}
std::uint32_t checksum(std::span<const std::uint8_t> data){
    std::uint32_t sum=0;for(std::size_t i=0;i<data.size();i+=4){std::uint32_t word=0;for(unsigned j=0;j<4;++j)word=(word<<8)|(i+j<data.size()?data[i+j]:0U);sum+=word;}return sum;
}
struct Table {std::uint32_t tag{};std::vector<std::uint8_t> data;};
// 單一字型（或 TTC 內指定字型）的表格。
std::vector<Table> read_tables(std::span<const std::uint8_t> file,bool collection,unsigned index){
    std::size_t base=0;
    if(be(file,0,4)==0x74746366U){ // 'ttcf'
        if(!collection)throw Failure{err_format};
        const auto count=be(file,8,4);if(index>=count)throw Failure{err_ttc_index};base=be(file,12+index*4,4);
    }else if(collection)throw Failure{err_not_ttc};
    const auto version=be(file,base,4);if(version!=0x00010000U && version!=0x74727565U && version!=0x4f54544fU)throw Failure{err_format};
    const auto count=be(file,base+4,2);std::vector<Table> tables;
    for(unsigned i=0;i<count;++i){
        const auto at=base+12+i*16;const auto offset=be(file,at+8,4),length=be(file,at+12,4);
        if(offset>file.size() || length>file.size()-offset)throw Failure{err_read_bounds};
        tables.push_back({be(file,at,4),std::vector<std::uint8_t>(file.begin()+offset,file.begin()+offset+length)});
    }
    return tables;
}
const Table* find(const std::vector<Table>& tables,std::uint32_t tag){for(const auto& t:tables)if(t.tag==tag)return &t;return nullptr;}
// cmap：依平台／編碼（0xffff 為不拘）挑子表，支援格式 0、4、6、12。
std::uint16_t map_character(const Table& cmap,std::uint32_t code,std::uint16_t platform,std::uint16_t encoding){
    const auto& d=cmap.data;const auto count=be(d,2,2);
    for(unsigned i=0;i<count;++i){
        const auto p=be(d,4+i*8,2),e=be(d,6+i*8,2);if((platform!=0xffff && p!=platform) || (encoding!=0xffff && e!=encoding))continue;
        const auto off=be(d,8+i*8,4);const auto format=be(d,off,2);
        if(format==0){if(code<256)return static_cast<std::uint16_t>(be(d,off+6+code,1));}
        else if(format==6){const auto first=be(d,off+6,2),n=be(d,off+8,2);if(code>=first && code<first+n)return static_cast<std::uint16_t>(be(d,off+10+(code-first)*2,2));}
        else if(format==4){
            const auto segs=be(d,off+6,2)/2;const auto ends=off+14,starts=ends+segs*2+2,deltas=starts+segs*2,ranges=deltas+segs*2;
            for(unsigned s=0;s<segs;++s){const auto end=be(d,ends+s*2,2),start=be(d,starts+s*2,2);if(code<start || code>end)continue;
                const auto delta=be(d,deltas+s*2,2),range=be(d,ranges+s*2,2);
                if(!range)return static_cast<std::uint16_t>((code+delta)&0xffff);
                const auto glyph=be(d,ranges+s*2+range+(code-start)*2,2);return glyph?static_cast<std::uint16_t>((glyph+delta)&0xffff):0;}
        }else if(format==12){
            const auto groups=be(d,off+12,4);
            for(std::uint32_t g=0;g<groups;++g){const auto at=off+16+g*12;const auto a=be(d,at,4),b=be(d,at+4,4);if(code>=a && code<=b)return static_cast<std::uint16_t>(be(d,at+8,4)+(code-a));}
        }else continue;
        return 0;
    }
    return 0;
}
}
static std::vector<std::uint8_t> font_package_subset(std::span<const std::uint8_t> file,std::uint16_t flags,std::uint16_t ttc_index,std::uint16_t format,
    std::uint16_t platform,std::uint16_t encoding,std::span<const std::uint16_t> keep,std::uint32_t& error){
    error=0;
    try{
        if(flags&~0x0fU)throw Failure{err_parameter+5};
        if(format>2)throw Failure{err_parameter+7};
        if(format==2)throw Failure{err_invalid_delta}; // 差量格式需搭配 MergeFontPackage，尚未提供。
        if(flags&2U)throw Failure{err_generic};          // MicroType 壓縮尚未提供。
        auto tables=read_tables(file,(flags&4U)!=0,ttc_index);
        const auto* head=find(tables,0x68656164U);if(!head)throw Failure{err_missing_head};
        const auto* maxp=find(tables,0x6d617870U);if(!maxp)throw Failure{err_missing_maxp};
        const auto* glyf=find(tables,0x676c7966U);if(!glyf)throw Failure{err_missing_glyf};
        const auto* loca=find(tables,0x6c6f6361U);if(!loca)throw Failure{err_missing_loca};
        const auto glyphs=be(maxp->data,4,2);const bool long_loca=static_cast<std::int16_t>(be(head->data,50,2))!=0;
        std::vector<std::uint32_t> offsets(glyphs+1U);
        for(unsigned i=0;i<=glyphs;++i)offsets[i]=long_loca?be(loca->data,i*4,4):be(loca->data,i*2,2)*2;
        for(unsigned i=0;i<glyphs;++i)if(offsets[i]>offsets[i+1] || offsets[i+1]>glyf->data.size())throw Failure{err_format};
        std::set<std::uint16_t> kept{0};
        if(flags&1U){
            const Table* cmap=nullptr;if(!(flags&8U)){cmap=find(tables,0x636d6170U);if(!cmap)throw Failure{err_missing_cmap};}
            for(const auto value:keep){const auto glyph=(flags&8U)?value:map_character(*cmap,value,platform,encoding);if(glyph<glyphs)kept.insert(glyph);}
            // 複合字形：加入所有元件字形，直到閉包穩定。
            std::vector<std::uint16_t> pending(kept.begin(),kept.end());
            while(!pending.empty()){
                const auto glyph=pending.back();pending.pop_back();const auto start=offsets[glyph],end=offsets[glyph+1];
                if(end-start<10 || static_cast<std::int16_t>(be(glyf->data,start,2))>=0)continue;
                for(std::size_t at=start+10;;){
                    const auto component_flags=be(glyf->data,at,2),component=be(glyf->data,at+2,2);at+=4;
                    if(component>=glyphs)throw Failure{err_format};
                    if(kept.insert(static_cast<std::uint16_t>(component)).second)pending.push_back(static_cast<std::uint16_t>(component));
                    at+=(component_flags&1U)?4:2;at+=(component_flags&8U)?2:(component_flags&0x40U)?4:(component_flags&0x80U)?8:0;
                    if(!(component_flags&0x20U))break;if(at>end)throw Failure{err_format};
                }
            }
        }else for(unsigned i=0;i<glyphs;++i)kept.insert(static_cast<std::uint16_t>(i));
        // 新 glyf／loca（長格式、4 bytes 對齊）。
        std::vector<std::uint8_t> new_glyf,new_loca((glyphs+1U)*4U);
        for(unsigned i=0;i<glyphs;++i){
            put(new_loca,i*4,static_cast<std::uint32_t>(new_glyf.size()),4);
            if(kept.contains(static_cast<std::uint16_t>(i))){new_glyf.insert(new_glyf.end(),glyf->data.begin()+offsets[i],glyf->data.begin()+offsets[i+1]);while(new_glyf.size()%4)new_glyf.push_back(0);}
        }
        put(new_loca,glyphs*4,static_cast<std::uint32_t>(new_glyf.size()),4);
        for(auto& table:tables){
            if(table.tag==0x676c7966U)table.data=new_glyf;
            else if(table.tag==0x6c6f6361U)table.data=new_loca;
            else if(table.tag==0x68656164U){if(table.data.size()<54)throw Failure{err_format};put(table.data,8,0,4);put(table.data,50,1,2);}
        }
        std::sort(tables.begin(),tables.end(),[](const auto& a,const auto& b){return a.tag<b.tag;});
        // 組成 sfnt：offset table、表格紀錄、4 bytes 對齊的表格資料，最後寫入 head.checkSumAdjustment。
        const auto count=static_cast<std::uint32_t>(tables.size());unsigned power=1,log2=0;while(power*2<=count){power*=2;++log2;}
        std::vector<std::uint8_t> out(12+count*16);put(out,0,0x00010000U,4);put(out,4,count,2);put(out,6,power*16,2);put(out,8,log2,2);put(out,10,count*16-power*16,2);
        std::size_t head_at=0;
        for(std::uint32_t i=0;i<count;++i){
            const auto& table=tables[i];const auto offset=out.size();if(table.tag==0x68656164U)head_at=offset;
            put(out,12+i*16,table.tag,4);put(out,16+i*16,checksum(table.data),4);put(out,20+i*16,static_cast<std::uint32_t>(offset),4);put(out,24+i*16,static_cast<std::uint32_t>(table.data.size()),4);
            out.insert(out.end(),table.data.begin(),table.data.end());while(out.size()%4)out.push_back(0);
        }
        put(out,head_at+8,0xb1b0afbaU-checksum(out),4);
        return out;
    }catch(const Failure& failure){error=failure.code;}catch(const std::bad_alloc&){error=err_mem;}
    return {};
}
std::span<const WindowsServiceDescriptor> font_package_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{{"fontsub.dll","CreateFontPackage",17,0,0,0,true}};return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_font_package(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto u16=[&](unsigned i){return static_cast<std::uint16_t>(arg(i));};
    const auto finish=[&](std::uint32_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    try{
        // 參數：src、src_len、dest、dest_size、bytes_written、flags、ttc_index、format、language、platform、encoding、keep、keep_count、alloc、realloc、free、reserved。
        if(!arg(0) || !static_cast<std::uint32_t>(arg(1)))return finish(err_parameter);
        if(!arg(2) || !arg(3) || !arg(4))return finish(err_parameter+2);
        if(!arg(13) || !arg(14) || !arg(15))return finish(err_parameter+13);
        const auto size=static_cast<std::uint32_t>(arg(1));if(size>64U*1024U*1024U)return finish(err_mem);
        const auto count=u16(12);if(count && !arg(11))return finish(err_parameter+11);
        std::vector<std::uint8_t> file(size);memory.read(GuestAddress{arg(0)},file);
        std::vector<std::uint16_t> keep(count);for(unsigned i=0;i<count;++i)keep[i]=static_cast<std::uint16_t>(memory.load(GuestAddress{arg(11)+i*2},16));
        std::uint32_t error=0;auto bytes=font_package_subset(file,u16(5),u16(6),u16(7),u16(9),u16(10),keep,error);if(error)return finish(error);
        const unsigned unit=x86_?4:8;memory.check(GuestAddress{arg(2)},unit,Access::Write);memory.check(GuestAddress{arg(3)},4,Access::Write);memory.check(GuestAddress{arg(4)},4,Access::Write);
        const auto id=next_font_package_++;font_packages_.emplace(id,FontPackageOutput{std::move(bytes),arg(2),arg(3),arg(4)});
        start_callbacks(state,memory,{{arg(13),{font_packages_.at(id).bytes.size()}}},Continuation::FontPackage,id);return std::nullopt;
    }catch(const MemoryFault&){return finish(err_read_bounds);}catch(const std::bad_alloc&){return finish(err_mem);}
}
std::optional<std::uint32_t> WindowsServices::resume_font_package(GuestState& state,GuestMemory& memory,std::uint64_t id,std::uint64_t value){
    const auto found=font_packages_.find(id);if(found==font_packages_.end()){complete_call(state,memory,err_generic);return std::nullopt;}
    auto output=std::move(found->second);font_packages_.erase(found);
    const auto pointer=x86_?value&0xffffffffU:value;if(!pointer){complete_call(state,memory,err_mem);return std::nullopt;}
    // allocproc 配置的區塊交給呼叫端，之後以其 freeproc 釋放。
    memory.write(GuestAddress{pointer},output.bytes);memory.store(GuestAddress{output.dest},x86_?32:64,pointer);
    memory.store(GuestAddress{output.dest_size},32,output.bytes.size());memory.store(GuestAddress{output.written},32,output.bytes.size());
    complete_call(state,memory,0);return std::nullopt;
}
void WindowsServices::discard_font_package(std::uint64_t id){font_packages_.erase(id);}
}
