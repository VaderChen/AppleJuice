#include "windows_esent_state.hpp"
#include <bit>

namespace x64arm {
namespace {
std::uint32_t column_flags(const esent::Column& column,bool updatable){
    const auto fid=column.id&0xffffU;std::uint32_t flags=fid<128?1U:fid>=256?2U:0U;
    constexpr std::pair<std::uint32_t,std::uint32_t> mapping[]={{1,4},{2,8},{4,16},{8,0x400},{0x20,0x800},{0x40,0x4000},{0x80,0x8000},{0x200,0x20000},{0x1000,0x80000},{0x2000,0x100000}};
    for(const auto [stored,public_flag]:mapping)if(column.flags&stored)flags|=public_flag;if(updatable)flags|=0x20;return flags;
}
struct Retrieval {esent::Binary bytes;std::uint32_t actual{},sequence{},next{};std::int32_t status{};};
Retrieval retrieve(const esent::Database& database,const esent::Table& table,const esent::Row& row,std::uint32_t id,std::uint32_t offset,std::uint32_t sequence,std::uint32_t flags,std::uint32_t capacity,bool count){
    using ese_api::require;require(!(flags&~0x3fU) && (flags&6U)!=6U,-900);require(id!=0 || !count,-1517);require(count || sequence!=0,-1518);
    Retrieval result;result.sequence=sequence;esent::Cell cell;const esent::Column* column{};
    const auto read_cell=[&](const esent::Column& c){
        require(!(c.flags&0x2080U),-1001);
        if((flags&0x20) && (c.id&0xffffU)>=256 && c.default_value){auto copy=table;copy.columns.at(c.id).default_value.reset();return esent::raw_cell(database,copy,row,c.id);}
        return esent::raw_cell(database,table,row,c.id);
    };
    if(id){
        require(!(flags&0x10),-900);const auto it=table.columns.find(id);require(it!=table.columns.end(),-1507);column=&it->second;cell=read_cell(*column);
    }else {
        // columnid 0 按已儲存的 tagged 欄位與值序號列舉，仍沿用相同解碼及 NULL 契約。
        std::uint32_t remaining=sequence;
        for(const auto& [cid,c]:table.columns){if((cid&0xffffU)<256)continue;auto candidate=read_cell(c);const auto values=candidate.is_null?((flags&0x10)?1U:0U):static_cast<std::uint32_t>(candidate.values.size());
            if(remaining>values){remaining-=values;continue;}column=&c;cell=std::move(candidate);result.next=cid;sequence=remaining;result.sequence=remaining;break;}
        if(!column){result.status=1004;result.next=0;return result;}
    }
    if(count){result.sequence=static_cast<std::uint32_t>(cell.values.size());return result;}
    if(cell.is_null || sequence>cell.values.size()){result.status=1004;return result;}
    result.bytes=esent::value(database,table,cell.values.at(sequence-1));
    if(column->type==11 || column->type==12){const auto skipped=std::min<std::size_t>(offset,result.bytes.size());result.bytes.erase(result.bytes.begin(),result.bytes.begin()+static_cast<std::ptrdiff_t>(skipped));}
    require(result.bytes.size()<=UINT32_MAX,-1011);result.actual=static_cast<std::uint32_t>(result.bytes.size());
    if(result.bytes.size()>capacity){result.bytes.resize(capacity);result.status=1006;}return result;
}
}

std::optional<std::uint32_t> WindowsServices::invoke_esent_cursor(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    using ese_api::require;const WindowsCall args(state,memory,service);const auto name=service.symbol;const bool wide=name.ends_with('W');const auto saved=last_error_;const std::uint8_t bits=x86_?32:64;
    const auto d=[&](unsigned i){return static_cast<std::uint32_t>(args(i));};const auto text=[&](unsigned i){return utf16_to_utf8(ese_api::input(memory,args(i),wide));};
    const auto done=[&](std::int32_t result){last_error_=saved;args.return_to(state,static_cast<std::uint32_t>(result));return std::optional<std::uint32_t>{};};
    auto& bridge=esent_bridge();auto& session=bridge.session(args(0));
    if(name.starts_with("JetOpenTable")){
        ese_api::writable(memory,args(6),bits/8);require(!args(3) && !d(4));auto& doc=bridge.document(args(0),d(1));const auto table_name=text(2);require(!table_name.empty() && table_name.size()<=255,-1002);
        const auto& table=esent::table(doc.data,table_name);auto flags=d(5);require(!(flags&~0xf80ffU) && (flags&12U)!=12U && (!(flags&16U) || (flags&2U)),-900);
        const bool readonly=(session.databases.at(d(1)).flags&1)!=0;require(!readonly || !(flags&8),-1008);if(readonly)flags|=4;
        for(const auto& [id,c]:bridge.cursors){(void)id;if(c.database!=d(1) || c.table!=table.name || c.session==args(0))continue;
            require(!(c.flags&2) && !(flags&2),-1302);require(!((c.flags&1) && !(flags&4)) && !((flags&1) && !(c.flags&4)),-1302);}
        EsentState::Cursor cursor;cursor.instance=session.instance;cursor.session=args(0);cursor.database=d(1);cursor.table=table.name;cursor.flags=flags;cursor.rows=esent::rows(doc.data,table);
        const auto id=bridge.add_cursor(std::move(cursor));memory.store(GuestAddress{args(6)},bits,id);return done(0);
    }
    if(name.starts_with("JetGetTableColumnInfo") || name.starts_with("JetGetColumnInfo")){
        const bool by_table=name.starts_with("JetGetTable");const unsigned shift=by_table?0U:1U;const esent::Table* table{};bool updatable{};
        if(by_table){const auto& c=bridge.cursor(args(0),args(1));table=&esent::table(bridge.documents.at(c.database).data,c.table);updatable=!(c.flags&4);}
        else {const auto& doc=bridge.document(args(0),d(1));table=&esent::table(doc.data,text(2));updatable=!(session.databases.at(d(1)).flags&1);}
        const auto level=d(5+shift);require(level==0 || level==4 || level==6 || level==8,-1001);const bool base=level==4 || level==8;
        const auto size=base?(wide?1052U:540U):28U;require(d(4+shift)>=size,-1038);ese_api::writable(memory,args(3+shift),size);const esent::Column* column{};
        if(level==6 || level==8){require(args(2+shift)!=0);const auto id=static_cast<std::uint32_t>(memory.load(GuestAddress{args(2+shift)},32));const auto it=table->columns.find(id);require(it!=table->columns.end(),-1507);column=&it->second;}
        else {const auto name_value=text(2+shift);require(!name_value.empty(),-1002);column=&esent::column(*table,name_value);}
        std::vector<std::uint8_t> bytes(size);const auto store=[&](std::size_t at,std::uint32_t value,unsigned width=4){for(unsigned i=0;i<width;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
        store(0,size);store(4,column->id);store(8,column->type);store(16,column->codepage,2);store(20,column->maximum);store(24,column_flags(*column,updatable));
        if(base){const auto put=[&](std::size_t at,const std::string& value){if(wide){const auto encoded=utf16_bytes(utf8_to_utf16(value).substr(0,255));std::copy(encoded.begin(),encoded.end(),bytes.begin()+static_cast<std::ptrdiff_t>(at));}
            else {std::copy_n(value.begin(),std::min<std::size_t>(255,value.size()),bytes.begin()+static_cast<std::ptrdiff_t>(at));}};
            put(28,column->inherited?table->template_name:table->name);put(28+(wide?512:256),column->name);}
        memory.write(GuestAddress{args(3+shift)},bytes);return done(0);
    }
    auto& cursor=bridge.cursor(args(0),args(1));
    if(name=="JetCloseTable"){bridge.cursors.erase(args(1));return done(0);}
    if(name=="JetDupCursor"){require(d(3)==0,-900);ese_api::writable(memory,args(2),bits/8);auto copy=cursor;copy.position=0;const auto id=bridge.add_cursor(std::move(copy));memory.store(GuestAddress{args(2)},bits,id);return done(0);}
    const auto count=static_cast<std::int64_t>(cursor.rows.size());
    const auto current=[&]()->const esent::Row& {require(cursor.position>=0 && cursor.position<count,-1603);return cursor.rows.at(static_cast<std::size_t>(cursor.position));};
    if(name=="JetMove"){
        require(!(d(3)&~1U),-900);const auto delta=std::bit_cast<std::int32_t>(d(2));
        if(delta==INT32_MIN)cursor.position=0;else if(delta==INT32_MAX)cursor.position=count-1;else cursor.position=std::clamp(cursor.position+delta,std::int64_t{-1},count);
        return done(cursor.position>=0 && cursor.position<count?0:-1603);
    }
    if(name=="JetGetBookmark"){
        const auto& key=current().key;require(args(2) || !d(3));if(args(4))ese_api::writable(memory,args(4),4);const auto copied=std::min<std::size_t>(d(3),key.size());
        if(copied)memory.write(GuestAddress{args(2)},std::span(key).first(copied));if(args(4))memory.store(GuestAddress{args(4)},32,key.size());return done(copied<key.size()?-1038:0);
    }
    if(name=="JetGotoBookmark"){
        require(args(2) && d(3) && d(3)<=32768,-1045);esent::Binary key(d(3));memory.read(GuestAddress{args(2)},key);
        const auto it=std::find_if(cursor.rows.begin(),cursor.rows.end(),[&](const auto& row){return row.key==key;});if(it==cursor.rows.end()){cursor.position=count;return done(-1601);}cursor.position=std::distance(cursor.rows.begin(),it);return done(0);
    }
    const auto& row=current();const auto& database=bridge.documents.at(cursor.database).data;const auto& table=esent::table(database,cursor.table);
    if(name=="JetRetrieveColumn"){
        require(args(3) || !d(4));if(args(5))ese_api::writable(memory,args(5),4);std::uint32_t offset=0,sequence=1;
        if(args(7)){memory.check(GuestAddress{args(7)},16,Access::Read);require(memory.load(GuestAddress{args(7)},32)>=16);offset=static_cast<std::uint32_t>(memory.load(GuestAddress{args(7)+4},32));sequence=static_cast<std::uint32_t>(memory.load(GuestAddress{args(7)+8},32));
            if(!d(2) || (d(6)&8))memory.check(GuestAddress{args(7)+8},8,Access::Write);}
        auto value=retrieve(database,table,row,d(2),offset,sequence,d(6),d(4),false);
        if(!value.bytes.empty())memory.write(GuestAddress{args(3)},value.bytes);if(args(5))memory.store(GuestAddress{args(5)},32,value.actual);
        if(args(7)){if(d(6)&8)memory.store(GuestAddress{args(7)+8},32,value.sequence);if(!d(2))memory.store(GuestAddress{args(7)+12},32,value.next);}return done(value.status);
    }
    if(name=="JetRetrieveColumns"){
        require(d(3)<=65536);if(!d(3))return done(0);const auto size=x86_?36U:48U;const auto start=x86_?8U:16U;ese_api::writable(memory,args(2),static_cast<std::size_t>(size)*d(3));
        struct Pending {std::uint64_t address{},buffer{};std::uint32_t column{},flags{};bool count{};Retrieval value;};std::vector<Pending> results;std::int32_t status=0;
        for(std::uint32_t i=0;i<d(3);++i){Pending p;p.address=args(2)+static_cast<std::uint64_t>(i)*size;
            const auto load=[&](unsigned offset){return static_cast<std::uint32_t>(memory.load(GuestAddress{p.address+offset},32));};
            p.column=load(0);p.buffer=memory.load(GuestAddress{p.address+(x86_?4U:8U)},bits);const auto capacity=load(start);p.flags=load(start+8);p.count=load(start+16)==0;require(p.buffer || !capacity);
            try{p.value=retrieve(database,table,row,p.column,load(start+12),load(start+16),p.flags,capacity,p.count);}
            catch(const ese_api::Error& error){memory.store(GuestAddress{p.address+start+24},32,static_cast<std::uint32_t>(error.code));return done(error.code);}
            if(!p.value.bytes.empty())memory.check(GuestAddress{p.buffer},p.value.bytes.size(),Access::Write);if(p.value.status==1006)status=1006;results.push_back(std::move(p));
        }
        for(const auto& p:results){if(!p.value.bytes.empty())memory.write(GuestAddress{p.buffer},p.value.bytes);memory.store(GuestAddress{p.address+start+4},32,p.value.actual);
            if(p.count || (p.flags&8))memory.store(GuestAddress{p.address+start+16},32,p.value.sequence);if(!p.column)memory.store(GuestAddress{p.address+start+20},32,p.value.next);memory.store(GuestAddress{p.address+start+24},32,static_cast<std::uint32_t>(p.value.status));}
        return done(status);
    }
    return done(-1003);
}
}
