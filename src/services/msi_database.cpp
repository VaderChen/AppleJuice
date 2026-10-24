#include "x64arm/msi_database.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <memory>
#include <unicode/ucnv.h>
#include <unicode/ucnv_err.h>

namespace x64arm::msi {
namespace {
constexpr std::array<std::uint8_t,16> database_clsid{0x84,0x10,0x0c,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
constexpr std::u16string_view alphabet=u"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._";
constexpr std::size_t max_rows=1000000,max_strings=1000000;
[[noreturn]] void corrupt(){throw Error{1627,"Windows Installer 資料庫格式無效"};}
std::uint32_t integer(std::span<const std::uint8_t> bytes,std::size_t offset,unsigned width){
    if(offset>bytes.size() || width>bytes.size()-offset)corrupt();
    std::uint32_t result=0;for(unsigned n=0;n<width;++n)result|=static_cast<std::uint32_t>(bytes[offset+n])<<(n*8);return result;
}
void append(Binary& bytes,std::uint32_t value,unsigned width){for(unsigned n=0;n<width;++n)bytes.push_back(static_cast<std::uint8_t>(value>>(n*8)));}
struct Codec {
    std::unique_ptr<UConverter,decltype(&ucnv_close)> converter{nullptr,ucnv_close};
    explicit Codec(std::uint32_t page){
        const auto encoding=page==65001?std::string("UTF-8"):page==20127?std::string("US-ASCII"):"windows-"+std::to_string(page);
        UErrorCode error=U_ZERO_ERROR;converter.reset(ucnv_open(encoding.c_str(),&error));
        if(U_FAILURE(error))throw Error{87,"MSI 字碼頁無效"};
        ucnv_setToUCallBack(converter.get(),UCNV_TO_U_CALLBACK_STOP,nullptr,nullptr,nullptr,&error);
        ucnv_setFromUCallBack(converter.get(),UCNV_FROM_U_CALLBACK_STOP,nullptr,nullptr,nullptr,&error);
        if(U_FAILURE(error))throw Error{1113,"MSI 字碼頁無法使用嚴格轉換"};
    }
    std::u16string decode(std::span<const std::uint8_t> bytes){
        if(bytes.size()>INT32_MAX)corrupt();
        UErrorCode error=U_ZERO_ERROR;const auto* data=reinterpret_cast<const char*>(bytes.data());
        const auto size=ucnv_toUChars(converter.get(),nullptr,0,data,static_cast<std::int32_t>(bytes.size()),&error);
        if(U_FAILURE(error) && error!=U_BUFFER_OVERFLOW_ERROR)throw Error{1113,"MSI 字串解碼失敗"};
        std::u16string out(static_cast<std::size_t>(size),0);error=U_ZERO_ERROR;
        ucnv_toUChars(converter.get(),out.data(),size,data,static_cast<std::int32_t>(bytes.size()),&error);
        if(U_FAILURE(error) || out.find(u'\0')!=out.npos)throw Error{1113,"MSI 字串包含無效內容"};return out;
    }
    Binary encode(std::u16string_view text){
        if(text.size()>INT32_MAX || text.find(u'\0')!=text.npos)throw Error{1113,"MSI 字串包含無效內容"};
        UErrorCode error=U_ZERO_ERROR;const auto size=ucnv_fromUChars(converter.get(),nullptr,0,text.data(),static_cast<std::int32_t>(text.size()),&error);
        if(U_FAILURE(error) && error!=U_BUFFER_OVERFLOW_ERROR)throw Error{1113,"MSI 字串無法表示於資料庫字碼頁"};
        Binary out(static_cast<std::size_t>(size));error=U_ZERO_ERROR;
        ucnv_fromUChars(converter.get(),reinterpret_cast<char*>(out.data()),size,text.data(),static_cast<std::int32_t>(text.size()),&error);
        if(U_FAILURE(error))throw Error{1113,"MSI 字串編碼失敗"};return out;
    }
};
const Binary* stream(const compound::Document& doc,std::u16string_view name,bool table){
    const auto id=doc.find(0,encode_stream_name(name,table));if(!id)return nullptr;
    const auto& node=doc.nodes.at(*id);if(node.kind!=compound::Kind::Stream)corrupt();return &node.data;
}
void store(compound::Document& doc,std::u16string_view name,bool table,Binary bytes){
    auto encoded=encode_stream_name(name,table);auto id=doc.find(0,encoded);
    if(!id)id=doc.add(0,std::move(encoded),compound::Kind::Stream);
    if(doc.nodes.at(*id).kind!=compound::Kind::Stream)corrupt();doc.nodes.at(*id).data=std::move(bytes);
}
unsigned width(const Column& column,unsigned refs){return column.binary()?2:column.text()?refs:(column.type&0xff)<=2?2:4;}
std::u16string binary_name(const Table& table,const Row& row){
    auto name=table.name;bool found=false;
    for(std::size_t i=0;i<table.columns.size();++i)if(table.columns[i].type&key){name+=u'.';name+=cell_text(row.fields.at(i));found=true;}
    if(!found)corrupt();return name;
}
void load_rows(Table& table,const compound::Document& storage,const std::vector<std::u16string>& strings,unsigned refs){
    const auto* bytes=stream(storage,table.name,true);if(!bytes)return;
    std::size_t row_width=0;for(const auto& column:table.columns)row_width+=width(column,refs);
    if(!row_width || bytes->size()%row_width)corrupt();
    const auto count=bytes->size()/row_width;if(count>max_rows)throw Error{8,"MSI 資料列超過資源上限"};
    table.rows.resize(count);for(auto& row:table.rows)row.fields.resize(table.columns.size());
    std::size_t offset=0;
    for(std::size_t c=0;c<table.columns.size();++c){
        const auto& column=table.columns[c];const auto size=width(column,refs);
        for(std::size_t r=0;r<count;++r){
            const auto value=integer(*bytes,offset+r*size,size);auto& cell=table.rows[r].fields[c];
            if(!value)continue;
            if(column.binary())cell=Binary{};
            else if(column.text()){if(value>=strings.size() || strings[value].empty())corrupt();cell=strings[value];}
            else cell=size==2?static_cast<std::int32_t>(value)-0x8000:std::bit_cast<std::int32_t>(value^0x80000000U);
        }
        offset+=count*size;
    }
    for(auto& row:table.rows)for(std::size_t c=0;c<table.columns.size();++c)if(table.columns[c].binary() && !std::holds_alternative<std::monostate>(row.fields[c])){
        const auto* value=stream(storage,binary_name(table,row),false);if(!value)corrupt();row.fields[c]=*value;
    }
}
std::u16string text_field(const Row& row,std::size_t index){
    const auto* value=std::get_if<std::u16string>(&row.fields.at(index));if(!value || value->empty())corrupt();return *value;
}
std::int32_t integer_field(const Row& row,std::size_t index){
    const auto* value=std::get_if<std::int32_t>(&row.fields.at(index));if(!value)corrupt();return *value;
}
}
std::u16string encode_stream_name(std::u16string_view text,bool table){
    std::u16string result;if(table)result.push_back(0x4840);
    for(std::size_t i=0;i<text.size();++i){
        const auto first=alphabet.find(text[i]);
        if(first==alphabet.npos){if(text[i]>=0x3800 && text[i]<=0x4840)throw Error{1627,"MSI 串流名稱使用保留字元"};result.push_back(text[i]);continue;}
        const auto second=i+1<text.size()?alphabet.find(text[i+1]):alphabet.npos;
        if(second!=alphabet.npos){result.push_back(static_cast<char16_t>(0x3800+first+64*second));++i;}
        else result.push_back(static_cast<char16_t>(0x4800+first));
    }
    if(!compound::valid_name(result))throw Error{1627,"MSI 串流名稱過長或無效"};return result;
}
std::u16string decode_stream_name(std::u16string_view text){
    std::u16string result;
    for(const auto c:text){
        if(c>=0x3800 && c<0x4800){result.push_back(alphabet[(c-0x3800)%64]);result.push_back(alphabet[(c-0x3800)/64]);}
        else if(c>=0x4800 && c<0x4840)result.push_back(alphabet[c-0x4800]);else result.push_back(c);
    }
    return result;
}
std::u16string cell_text(const Cell& cell){
    if(const auto* value=std::get_if<std::u16string>(&cell))return *value;
    if(const auto* value=std::get_if<std::int32_t>(&cell)){const auto str=std::to_string(*value);return {str.begin(),str.end()};}
    return {};
}
void validate(const Table& table){
    if(table.name.empty() || table.name.size()>31 || table.columns.empty() || table.columns.size()>32)throw Error{1627,"MSI 資料表結構無效"};
    (void)encode_stream_name(table.name,true);std::set<std::u16string> names;std::vector<std::size_t> keys;unsigned binaries=0;
    for(std::size_t i=0;i<table.columns.size();++i){
        const auto& column=table.columns[i];const auto size=column.type&0xff;
        if(column.name.empty() || column.name.size()>64 || !names.insert(column.name).second || !(column.type&valid) ||
           (!column.text() && !column.binary() && size!=2 && size!=4))throw Error{1627,"MSI 欄位結構無效"};
        if(column.type&key){if(column.binary() || keys.size()!=i)throw Error{1627,"MSI 主鍵須依優先順序排列於前端且不可為二進位"};keys.push_back(i);}
        if(i && (table.columns[i-1].type&temporary) && !(column.type&temporary))throw Error{1627,"MSI 持久欄位必須在暫存欄位之前"};
        if(column.binary() && ++binaries>1)throw Error{1627,"MSI 資料表僅能包含一個二進位欄位"};
    }
    if(keys.empty())throw Error{1627,"MSI 資料表缺少主鍵"};
    std::set<std::vector<Cell>> unique;
    for(const auto& row:table.rows){
        if(row.fields.size()!=table.columns.size())corrupt();
        for(std::size_t i=0;i<row.fields.size();++i){
            const auto& column=table.columns[i];const auto& cell=row.fields[i];
            if(std::holds_alternative<std::monostate>(cell)){if(!(column.type&nullable) || (column.type&key))throw Error{1627,"MSI 必填欄位不可為 NULL"};continue;}
            if(column.text()){
                const auto* value=std::get_if<std::u16string>(&cell);if(!value || value->empty() || ((column.type&0xff) && value->size()>(column.type&0xff)))throw Error{1627,"MSI 字串欄位長度或種類無效"};
            }else if(column.binary()){if(!std::holds_alternative<Binary>(cell))throw Error{1627,"MSI 二進位欄位種類無效"};}
            else {
                const auto* value=std::get_if<std::int32_t>(&cell);
                if(!value || *value==INT32_MIN || ((column.type&0xff)<=2 && (*value<-32767 || *value>32767)))throw Error{1627,"MSI 整數欄位超出範圍"};
            }
        }
        std::vector<Cell> values;for(const auto index:keys)values.push_back(row.fields[index]);
        if(!unique.insert(std::move(values)).second)throw Error{1627,"MSI 主鍵重複"};
    }
}
Table table_catalog(const Database& db){
    Table out{u"_Tables",{{u"Name",valid|string|key|64}}, {}};
    for(const auto& [name,table]:db.tables){(void)table;out.rows.push_back({{name}});}return out;
}
Table column_catalog(const Database& db){
    Table out{u"_Columns",{{u"Table",valid|string|key|64},{u"Number",valid|key|2},{u"Name",valid|string|64},{u"Type",valid|2}}, {}};
    for(const auto& [name,table]:db.tables)for(std::size_t i=0;i<table.columns.size();++i){const auto& col=table.columns[i];out.rows.push_back({{name,static_cast<std::int32_t>(i+1),col.name,static_cast<std::int32_t>(col.type)}});}
    return out;
}
Table stream_catalog(const Database& db){
    std::map<std::u16string,Binary> streams;
    for(const auto id:db.storage.children(0)){const auto& node=db.storage.nodes.at(id);if(node.kind==compound::Kind::Stream && !node.name.empty() && node.name.front()!=0x4840)streams.emplace(decode_stream_name(node.name),node.data);}
    for(const auto& name:db.binary_streams)streams.erase(decode_stream_name(name));
    for(const auto& [name,table]:db.tables){(void)name;for(const auto& row:table.rows)for(std::size_t c=0;c<table.columns.size();++c)if(table.columns[c].binary())if(const auto* data=std::get_if<Binary>(&row.fields[c]))streams[binary_name(table,row)]=*data;}
    Table out{u"_Streams",{{u"Name",valid|string|0x400|key|62},{u"Data",valid|string|nullable}}, {}};
    for(auto& [name,data]:streams)out.rows.push_back({{name,std::move(data)}});return out;
}
std::map<std::u16string,std::uint32_t> string_indices(const Database& db){
    auto ids=db.string_ids;std::uint32_t next=1;std::set<std::uint32_t> used;
    for(const auto& [text,id]:ids){if(text.empty() || !id || id>=max_strings || !used.insert(id).second)corrupt();next=std::max(next,id+1);}
    const auto add=[&](const Table& table){for(const auto& row:table.rows)for(const auto& cell:row.fields)if(const auto* text=std::get_if<std::u16string>(&cell))if(!text->empty() && !ids.contains(*text)){if(next>=max_strings)throw Error{8,"MSI 字串表超過資源上限"};ids.emplace(*text,next++);}};
    for(const auto& [name,table]:db.tables){(void)name;add(table);}add(table_catalog(db));add(column_catalog(db));
    add(stream_catalog(db));
    return ids;
}
Database create(std::uint32_t codepage,std::uint32_t fallback){
    Database db;db.codepage=codepage;db.default_codepage=fallback;db.storage.nodes.at(0).clsid=database_clsid;return db;
}
Database parse(std::span<const std::uint8_t> bytes,std::uint32_t fallback,std::size_t limit){
    Database db=create(0,fallback);db.storage=compound::parse(bytes,limit);
    const auto clsid=db.storage.nodes.at(0).clsid;
    // .msi／.msp 共用資料表儲存；不把 transform 的差異列當成完整資料表。
    if(clsid!=database_clsid && !(clsid[0]==0x86 && std::equal(clsid.begin()+1,clsid.end(),database_clsid.begin()+1)))corrupt();
    const auto* pool=stream(db.storage,u"_StringPool",true);const auto* data=stream(db.storage,u"_StringData",true);
    if(!pool || !data || pool->size()<4 || pool->size()%4)corrupt();
    db.codepage=integer(*pool,0,4)&0x7fffffffU;const unsigned refs=(integer(*pool,0,4)&0x80000000U)?3:2;
    Codec codec(db.codepage?db.codepage:fallback);std::vector<std::u16string> strings(1);std::size_t offset=0;
    for(std::size_t at=4;at<pool->size();){
        auto length=integer(*pool,at,2);const auto references=integer(*pool,at+2,2);at+=4;
        if(!length && references){length=integer(*pool,at,4);at+=4;}
        if(offset>data->size() || length>data->size()-offset || strings.size()>=max_strings)corrupt();
        strings.push_back(length?codec.decode(std::span(*data).subspan(offset,length)):std::u16string{});offset+=length;
        if(!strings.back().empty())db.string_ids.try_emplace(strings.back(),static_cast<std::uint32_t>(strings.size()-1));
    }
    if(offset!=data->size())corrupt();
    auto names=table_catalog(db),columns=column_catalog(db);load_rows(names,db.storage,strings,refs);load_rows(columns,db.storage,strings,refs);
    for(const auto& row:names.rows){const auto name=text_field(row,0);if(name==u"_Tables" || name==u"_Columns" || !db.tables.emplace(name,Table{name,{},{}}).second)corrupt();}
    for(const auto& row:columns.rows){
        const auto name=text_field(row,0);const auto found=db.tables.find(name);if(found==db.tables.end())corrupt();
        const auto number=integer_field(row,1),type=integer_field(row,3);if(number<1 || number>32 || type<=0 || type>0x7fff)corrupt();
        auto& table=found->second;table.columns.resize(std::max(table.columns.size(),static_cast<std::size_t>(number)));
        if(!table.columns[static_cast<std::size_t>(number-1)].name.empty())corrupt();
        table.columns[static_cast<std::size_t>(number-1)]={text_field(row,2),static_cast<std::uint16_t>(type)};
    }
    for(auto& [name,table]:db.tables){
        (void)name;validate(table);load_rows(table,db.storage,strings,refs);validate(table);
        for(const auto& row:table.rows)for(std::size_t c=0;c<table.columns.size();++c)
            if(table.columns[c].binary() && std::holds_alternative<Binary>(row.fields[c]))db.binary_streams.insert(encode_stream_name(binary_name(table,row),false));
    }
    return db;
}
std::vector<std::uint8_t> serialize(const Database& source,std::size_t limit){
    Database db=source;db.tables.clear();
    for(const auto& [name,original]:source.tables){
        validate(original);if(!original.persistent)continue;Table table{name,{},{}};std::vector<std::size_t> keep;
        for(std::size_t i=0;i<original.columns.size();++i)if(!(original.columns[i].type&temporary)){keep.push_back(i);table.columns.push_back(original.columns[i]);}
        if(keep.empty())continue;
        for(const auto& row:original.rows)if(!row.temporary){Row copy;for(const auto index:keep)copy.fields.push_back(row.fields[index]);table.rows.push_back(std::move(copy));}
        validate(table);db.tables.emplace(name,std::move(table));
    }
    auto tables=db.tables;tables.emplace(u"_Tables",table_catalog(db));tables.emplace(u"_Columns",column_catalog(db));
    const auto ids=string_indices(db);std::map<std::u16string,std::uint32_t> counts;std::vector<std::u16string> strings(1);
    for(const auto& [text,id]:ids){if(strings.size()<=id)strings.resize(static_cast<std::size_t>(id)+1);strings[id]=text;}
    for(auto& [name,table]:tables){(void)name;std::stable_sort(table.rows.begin(),table.rows.end(),[&](const auto& a,const auto& b){
        for(std::size_t i=0;i<table.columns.size() && (table.columns[i].type&key);++i){if(a.fields[i]==b.fields[i])continue;
            if(table.columns[i].text())return ids.at(std::get<std::u16string>(a.fields[i]))<ids.at(std::get<std::u16string>(b.fields[i]));return a.fields[i]<b.fields[i];}return false;});}
    for(const auto& [name,table]:tables)for(const auto& row:table.rows)for(std::size_t c=0;c<table.columns.size();++c){
        (void)name;if(!table.columns[c].text())continue;
        if(const auto* value=std::get_if<std::u16string>(&row.fields[c])){
            ++counts[*value];
        }
    }
    const unsigned refs=strings.size()>0x10000?3:2;Codec codec(db.codepage?db.codepage:db.default_codepage);
    Binary pool,data;append(pool,db.codepage|(refs==3?0x80000000U:0),4);
    for(std::size_t i=1;i<strings.size();++i){
        // 保留既有 ID 與空洞；未使用字串不攜帶 payload，也不改變其他欄位排序。
        const auto references=std::min(counts[strings[i]],0xffffU);
        if(!references){append(pool,0,4);continue;}
        const auto bytes=codec.encode(strings[i]);if(bytes.size()>UINT32_MAX || data.size()>limit || bytes.size()>limit-data.size())throw Error{8,"MSI 字串表超過資源上限"};
        if(bytes.size()>0xffff){append(pool,0,2);append(pool,references,2);append(pool,static_cast<std::uint32_t>(bytes.size()),4);}
        else {append(pool,static_cast<std::uint32_t>(bytes.size()),2);append(pool,references,2);}
        data.insert(data.end(),bytes.begin(),bytes.end());
    }
    auto storage=db.storage;
    for(const auto id:storage.children(0)){const auto& node=storage.nodes.at(id);if(node.kind==compound::Kind::Stream && ((!node.name.empty() && node.name.front()==0x4840) || db.binary_streams.contains(node.name)))storage.erase(id);}
    store(storage,u"_StringPool",true,std::move(pool));store(storage,u"_StringData",true,std::move(data));
    for(const auto& [name,table]:tables){
        Binary bytes;
        for(std::size_t c=0;c<table.columns.size();++c){
            const auto& column=table.columns[c];const auto size=width(column,refs);
            for(const auto& row:table.rows){
                const auto& cell=row.fields[c];std::uint32_t value=0;
                if(const auto* text=std::get_if<std::u16string>(&cell))value=ids.at(*text);
                else if(const auto* number=std::get_if<std::int32_t>(&cell))value=size==2?static_cast<std::uint32_t>(*number+0x8000):std::bit_cast<std::uint32_t>(*number)^0x80000000U;
                else if(const auto* binary=std::get_if<Binary>(&cell)){value=1;store(storage,binary_name(table,row),false,*binary);}
                append(bytes,value,size);
                if(bytes.size()>limit)throw Error{8,"MSI 資料表超過資源上限"};
            }
        }
        store(storage,name,true,std::move(bytes));
    }
    return compound::serialize(storage,limit);
}
}
