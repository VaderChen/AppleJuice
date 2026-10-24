#include "x64arm/msi_query.hpp"
#include <algorithm>
#include <numeric>

namespace x64arm::msi {
namespace {
[[noreturn]] void syntax(){throw Error{1615,"Windows Installer SQL 無效"};}
[[noreturn]] void failure(){throw Error{1627,"Windows Installer SQL 操作失敗"};}
constexpr std::uint16_t text_type=valid|string|0x400;
constexpr std::size_t result_limit=1000000;
bool system_table(std::u16string_view name){return name==u"_Tables" || name==u"_Columns" || name==u"_Streams" || name==u"_Storages" || name==u"_StringPool" || name==u"_StringData";}
std::u16string upper(std::u16string text){for(auto& c:text)if(c>=u'a' && c<=u'z')c-=32;return text;}
bool digit(char16_t c){return c>=u'0' && c<=u'9';}
enum class TokenKind { End, Word, Identifier, Text, Number, Mark };
struct Token {TokenKind kind{};std::u16string value;};
std::vector<Token> tokenize(std::u16string_view sql){
    if(sql.empty() || sql.size()>65536)syntax();std::vector<Token> tokens;
    for(std::size_t i=0;i<sql.size();){
        auto c=sql[i];if(c==u' ' || c==u'\r' || c==u'\n' || c==u'\t'){++i;continue;}
        if(c==u'`' || c==u'\''){
            const auto end=sql.find(c,i+1);if(end==sql.npos)syntax();
            tokens.push_back({c==u'`'?TokenKind::Identifier:TokenKind::Text,std::u16string(sql.substr(i+1,end-i-1))});i=end+1;
        }else if(digit(c) || (c==u'-' && i+1<sql.size() && digit(sql[i+1]))){
            const auto start=i++;while(i<sql.size() && digit(sql[i]))++i;tokens.push_back({TokenKind::Number,std::u16string(sql.substr(start,i-start))});
        }else if(std::u16string_view(u"(),.*?=<>;").find(c)!=std::u16string_view::npos){
            std::u16string value(1,c);++i;if(i<sql.size() && ((c==u'<' && (sql[i]==u'>' || sql[i]==u'=')) || (c==u'>' && sql[i]==u'=')))value+=sql[i++];
            tokens.push_back({TokenKind::Mark,std::move(value)});
        }else{
            const auto word=[](char16_t x){return (x>=u'a' && x<=u'z') || (x>=u'A' && x<=u'Z') || digit(x) || x==u'_' || x>=128;};
            if(!word(c))syntax();const auto start=i++;while(i<sql.size() && word(sql[i]))++i;
            tokens.push_back({TokenKind::Word,std::u16string(sql.substr(start,i-start))});
        }
        if(tokens.size()>32768)syntax();
    }
    tokens.push_back({TokenKind::End,{}});return tokens;
}
struct Reference {std::u16string table,name;};
struct Operand {
    enum class Kind { Field, Literal, Parameter } kind{Kind::Literal};
    Reference reference;Cell value;std::size_t parameter{},column{};
};
struct Predicate {
    enum class Kind { Equal, NotEqual, Less, Greater, LessEqual, GreaterEqual, Null, NotNull, And, Or } kind{};
    Operand left,right;std::size_t first{},second{};
};
enum class Action {Select,Insert,Update,Delete,Create,Drop,Add,Hold,Free};
struct Statement {
    Action action{};std::vector<std::u16string> tables;std::vector<Reference> fields,order;
    std::vector<Operand> values;std::vector<Predicate> predicates;std::optional<std::size_t> predicate;
    std::vector<Column> definitions;bool star{},distinct{},temporary_row{},hold{};std::size_t parameters{};
};
class Parser {
    std::vector<Token> tokens;std::size_t position{},depth{},conditions{};
    Statement statement;
    const Token& peek()const{return tokens.at(position);}
    bool take(std::u16string_view value){if((peek().kind==TokenKind::Word && upper(peek().value)==value) || (peek().kind==TokenKind::Mark && peek().value==value)){++position;return true;}return false;}
    void need(std::u16string_view value){if(!take(value))syntax();}
    std::u16string identifier(){if(peek().kind!=TokenKind::Word && peek().kind!=TokenKind::Identifier)syntax();auto value=tokens[position++].value;if(value.empty())syntax();return value;}
    Reference field(){Reference ref{{},identifier()};if(take(u".")){ref.table=std::move(ref.name);ref.name=identifier();}return ref;}
    void table_list(){do{statement.tables.push_back(identifier());}while(take(u","));if(statement.tables.size()>32)syntax();}
    Operand constant(){
        Operand out;
        if(take(u"?")){out.kind=Operand::Kind::Parameter;out.parameter=statement.parameters++;}
        else if(take(u"NULL")){}
        else if(peek().kind==TokenKind::Text){auto value=tokens[position++].value;if(!value.empty())out.value=std::move(value);}
        else if(peek().kind==TokenKind::Number){auto value=cell_integer(Cell{tokens[position++].value});if(!value || *value==INT32_MIN)syntax();out.value=*value;}
        else syntax();return out;
    }
    Operand operand(){
        if(peek().kind==TokenKind::Identifier || (peek().kind==TokenKind::Word && upper(peek().value)!=u"NULL")){Operand out;out.kind=Operand::Kind::Field;out.reference=field();return out;}
        return constant();
    }
    std::size_t atom(){
        if(++depth>64)syntax();std::size_t index;
        if(take(u"(")){index=expression();need(u")");}
        else {
            if(++conditions>32)syntax();Predicate test;test.left=operand();if(test.left.kind!=Operand::Kind::Field)syntax();
            if(take(u"IS")){const bool not_null=take(u"NOT");need(u"NULL");test.kind=not_null?Predicate::Kind::NotNull:Predicate::Kind::Null;}
            else {
                if(take(u"="))test.kind=Predicate::Kind::Equal;else if(take(u"<>"))test.kind=Predicate::Kind::NotEqual;
                else if(take(u"<"))test.kind=Predicate::Kind::Less;else if(take(u">"))test.kind=Predicate::Kind::Greater;
                else if(take(u"<="))test.kind=Predicate::Kind::LessEqual;else if(take(u">="))test.kind=Predicate::Kind::GreaterEqual;else syntax();
                test.right=operand();
            }
            index=statement.predicates.size();statement.predicates.push_back(std::move(test));
        }
        --depth;return index;
    }
    std::size_t conjunction(){auto left=atom();while(take(u"AND")){const auto right=atom();statement.predicates.push_back({Predicate::Kind::And,{},{},left,right});left=statement.predicates.size()-1;}return left;}
    std::size_t expression(){auto left=conjunction();while(take(u"OR")){const auto right=conjunction();statement.predicates.push_back({Predicate::Kind::Or,{},{},left,right});left=statement.predicates.size()-1;}return left;}
    void where(){if(take(u"WHERE"))statement.predicate=expression();}
    Column definition(){
        Column col{identifier(),nullable};
        if(take(u"CHAR") || take(u"CHARACTER")){
            col.type|=text_type;
            if(take(u"(")){const auto value=constant();const auto* size=std::get_if<std::int32_t>(&value.value);if(!size || *size<0 || *size>255)syntax();col.type|=static_cast<std::uint16_t>(*size);need(u")");}
        }else if(take(u"LONGCHAR"))col.type|=text_type;
        else if(take(u"SHORT") || take(u"INT") || take(u"INTEGER"))col.type|=valid|0x400|2;
        else if(take(u"LONG"))col.type|=valid|4;
        else if(take(u"OBJECT"))col.type|=valid|string;
        else syntax();
        if(take(u"NOT")){need(u"NULL");col.type&=~nullable;}
        if(take(u"TEMPORARY"))col.type|=temporary;
        if(take(u"LOCALIZABLE")){if(!col.text())syntax();col.type|=localizable;}
        return col;
    }
    void parameter_order(){bool literal=false;for(const auto& item:statement.values){if(item.kind==Operand::Kind::Parameter){if(literal)syntax();}else literal=true;}}
public:
    explicit Parser(std::u16string_view sql):tokens(tokenize(sql)){}
    Statement parse(){
        if(take(u"SELECT")){
            statement.action=Action::Select;statement.distinct=take(u"DISTINCT");statement.star=take(u"*");
            if(!statement.star)do{statement.fields.push_back(field());}while(take(u","));
            need(u"FROM");table_list();where();if(take(u"ORDER")){need(u"BY");do{statement.order.push_back(field());}while(take(u","));}
        }else if(take(u"INSERT")){
            statement.action=Action::Insert;need(u"INTO");statement.tables.push_back(identifier());need(u"(");
            do{statement.fields.push_back(field());}while(take(u","));need(u")");need(u"VALUES");need(u"(");
            do{statement.values.push_back(constant());}while(take(u","));need(u")");statement.temporary_row=take(u"TEMPORARY");parameter_order();
        }else if(take(u"UPDATE")){
            statement.action=Action::Update;table_list();need(u"SET");
            do{statement.fields.push_back(field());need(u"=");statement.values.push_back(constant());}while(take(u","));parameter_order();where();
        }else if(take(u"DELETE")){statement.action=Action::Delete;need(u"FROM");statement.tables.push_back(identifier());where();}
        else if(take(u"CREATE")){
            statement.action=Action::Create;need(u"TABLE");statement.tables.push_back(identifier());need(u"(");
            do{statement.definitions.push_back(definition());}while(take(u","));need(u"PRIMARY");need(u"KEY");
            std::set<std::u16string> keys;
            do{const auto name=identifier();if(!keys.insert(name).second || keys.size()>statement.definitions.size())syntax();auto& col=statement.definitions[keys.size()-1];if(col.name!=name)syntax();col.type|=key;}while(take(u","));
            need(u")");statement.hold=take(u"HOLD");
        }else if(take(u"DROP")){statement.action=Action::Drop;need(u"TABLE");statement.tables.push_back(identifier());}
        else if(take(u"ALTER")){
            need(u"TABLE");statement.tables.push_back(identifier());
            if(take(u"ADD")){statement.action=Action::Add;statement.definitions.push_back(definition());statement.hold=take(u"HOLD");}
            else if(take(u"HOLD"))statement.action=Action::Hold;else if(take(u"FREE"))statement.action=Action::Free;else syntax();
        }else syntax();
        (void)take(u";");if(peek().kind!=TokenKind::End)syntax();return std::move(statement);
    }
};
struct Binding {
    Statement statement;std::vector<Table> tables;std::vector<Column> columns;
    std::vector<std::size_t> selected,ordered,table_bases;
    std::size_t find(const Reference& ref)const{
        std::optional<std::size_t> found;std::size_t base=0;
        for(const auto& table:tables){if(ref.table.empty() || ref.table==table.name)for(std::size_t c=0;c<table.columns.size();++c)if(ref.name==table.columns[c].name){if(found)syntax();found=base+c;}base+=table.columns.size();}
        if(!found)syntax();return *found;
    }
};
Binding bind(const Database& db,Statement statement){
    Binding out;out.statement=std::move(statement);auto& s=out.statement;
    if(s.action==Action::Create){
        if(db.tables.contains(s.tables[0]) || system_table(s.tables[0]))syntax();
        Table table{s.tables[0],s.definitions,{}};validate(table);return out;
    }
    std::set<std::u16string> names;
    for(const auto& name:s.tables){
        if(!names.insert(name).second)syntax();
        if(name==u"_Tables")out.tables.push_back(table_catalog(db));else if(name==u"_Columns")out.tables.push_back(column_catalog(db));
        else if(name==u"_Streams")out.tables.push_back(stream_catalog(db));else {const auto found=db.tables.find(name);if(found==db.tables.end())syntax();out.tables.push_back(found->second);}
        out.table_bases.push_back(out.columns.size());const auto& cols=out.tables.back().columns;out.columns.insert(out.columns.end(),cols.begin(),cols.end());
    }
    if(s.action!=Action::Select && std::any_of(s.tables.begin(),s.tables.end(),[](const auto& n){return system_table(n);}))syntax();
    if(s.star){out.selected.resize(out.columns.size());std::iota(out.selected.begin(),out.selected.end(),0);}
    else for(const auto& field:s.fields)out.selected.push_back(out.find(field));
    for(const auto& field:s.order)out.ordered.push_back(out.find(field));
    if((s.action==Action::Insert || s.action==Action::Update) && (out.selected.size()!=s.values.size() || std::set<std::size_t>(out.selected.begin(),out.selected.end()).size()!=out.selected.size()))syntax();
    if(s.action==Action::Update)for(const auto index:out.selected)if(out.columns[index].type&key)syntax();
    std::vector<std::size_t> components(out.tables.size());std::iota(components.begin(),components.end(),0);std::set<std::pair<std::size_t,std::size_t>> joins;
    for(auto& p:s.predicates){
        if(p.kind==Predicate::Kind::And || p.kind==Predicate::Kind::Or)continue;
        p.left.column=out.find(p.left.reference);
        if(p.kind==Predicate::Kind::Null || p.kind==Predicate::Kind::NotNull)continue;
        const auto& col=out.columns[p.left.column];
        if(col.binary() || (col.text() && p.kind!=Predicate::Kind::Equal && p.kind!=Predicate::Kind::NotEqual))syntax();
        if(p.right.kind==Operand::Kind::Field){
            if(p.kind!=Predicate::Kind::Equal)syntax();p.right.column=out.find(p.right.reference);const auto& right=out.columns[p.right.column];if(col.text()!=right.text() || col.binary()!=right.binary())syntax();
            const auto owner=[&](std::size_t column){return static_cast<std::size_t>(std::upper_bound(out.table_bases.begin(),out.table_bases.end(),column)-out.table_bases.begin()-1);};
            const auto a=owner(p.left.column),b=owner(p.right.column);
            if(a!=b && joins.insert(std::minmax(a,b)).second){if(components[a]==components[b])syntax();const auto from=components[b],to=components[a];for(auto& component:components)if(component==from)component=to;}
        }
    }
    if(s.action==Action::Add){auto table=out.tables[0];table.columns.push_back(s.definitions[0]);table.rows.clear();validate(table);}
    return out;
}
Cell value(const Operand& operand,std::span<const Cell> row,std::span<const Cell> parameters){
    if(operand.kind==Operand::Kind::Field)return row[operand.column];
    if(operand.kind==Operand::Kind::Parameter){if(operand.parameter>=parameters.size())throw Error{87,"MSI SQL 缺少參數"};return parameters[operand.parameter];}
    return operand.value;
}
Cell convert(const Column& col,Cell cell){
    if(std::holds_alternative<std::monostate>(cell))return cell;
    if(col.binary()){if(!std::holds_alternative<Binary>(cell))failure();return cell;}
    if(col.text()){if(std::holds_alternative<Binary>(cell))failure();auto text=cell_text(cell);return text.empty()?Cell{}:Cell{std::move(text)};}
    const auto integer=cell_integer(cell);if(!integer || *integer==INT32_MIN)failure();return *integer;
}
bool matches(const Binding& binding,std::span<const Cell> row,std::span<const Cell> parameters){
    if(!binding.statement.predicate)return true;std::vector<bool> values;
    for(const auto& p:binding.statement.predicates){
        using K=Predicate::Kind;bool result=false;
        if(p.kind==K::And)result=values[p.first] && values[p.second];
        else if(p.kind==K::Or)result=values[p.first] || values[p.second];
        else {
            const auto left=value(p.left,row,parameters);const bool null=std::holds_alternative<std::monostate>(left);
            if(p.kind==K::Null)result=null;else if(p.kind==K::NotNull)result=!null;
            else {
                const auto right=convert(binding.columns[p.left.column],value(p.right,row,parameters));
                if(!null && !std::holds_alternative<std::monostate>(right)){
                    if(p.kind==K::Equal)result=left==right;else if(p.kind==K::NotEqual)result=left!=right;
                    else if(p.kind==K::Less)result=left<right;else if(p.kind==K::Greater)result=left>right;
                    else if(p.kind==K::LessEqual)result=left<=right;else if(p.kind==K::GreaterEqual)result=left>=right;
                }
            }
        }
        values.push_back(result);
    }
    return values[*binding.statement.predicate];
}
struct Joined {std::vector<Cell> fields;std::vector<std::size_t> indices;};
std::vector<Joined> joined(const Binding& binding,std::span<const Cell> parameters){
    std::vector<Joined> rows(1);
    for(const auto& table:binding.tables){
        if(table.rows.size() && rows.size()>result_limit/table.rows.size())throw Error{8,"MSI SQL 資料組合超過資源上限"};
        std::vector<Joined> next;next.reserve(rows.size()*table.rows.size());
        for(const auto& base:rows)for(std::size_t i=0;i<table.rows.size();++i){auto row=base;const auto& extra=table.rows[i].fields;row.fields.insert(row.fields.end(),extra.begin(),extra.end());row.indices.push_back(i);next.push_back(std::move(row));}
        rows=std::move(next);
    }
    std::erase_if(rows,[&](const auto& row){return !matches(binding,row.fields,parameters);});return rows;
}
void free_temporary(Database& db,const std::u16string& name){
    auto& table=db.tables.at(name);if(table.holds)--table.holds;if(table.holds)return;
    for(std::size_t i=table.columns.size();i-->0;)if(table.columns[i].type&temporary){table.columns.erase(table.columns.begin()+static_cast<std::ptrdiff_t>(i));for(auto& row:table.rows)row.fields.erase(row.fields.begin()+static_cast<std::ptrdiff_t>(i));}
    if(table.columns.empty())db.tables.erase(name);else validate(table);
}
}
std::optional<std::int32_t> cell_integer(const Cell& cell){
    if(const auto* number=std::get_if<std::int32_t>(&cell))return *number;
    const auto* text=std::get_if<std::u16string>(&cell);if(!text || text->empty())return {};
    std::size_t at=0;bool negative=false;if((*text)[0]==u'-'){negative=true;++at;}if(at==text->size())return {};
    const std::uint64_t limit=negative?2147483648ULL:2147483647ULL;std::uint64_t value=0;
    for(;at<text->size();++at){const auto c=(*text)[at];if(!digit(c))return {};const auto d=static_cast<unsigned>(c-u'0');if(value>(limit-d)/10)return {};value=value*10+d;}
    return static_cast<std::int32_t>(negative?-static_cast<std::int64_t>(value):static_cast<std::int64_t>(value));
}
std::u16string column_definition(const Column& col){
    char16_t type=col.binary()?u'v':col.text()?((col.type&localizable)?u'l':u's'):u'i';if(col.type&nullable)type-=32;
    const auto width=std::to_string(col.binary()?0:col.type&0xff);return std::u16string(1,type)+std::u16string(width.begin(),width.end());
}
Query prepare(const Database& db,std::u16string_view sql){
    const auto bound=bind(db,Parser(sql).parse());Query query{std::u16string(sql),{},bound.statement.parameters,bound.statement.action!=Action::Select};
    if(!query.writes)for(const auto index:bound.selected)query.columns.push_back(bound.columns[index]);return query;
}
Result execute(Database& database,const Query& query,std::span<const Cell> parameters){
    const auto binding=bind(database,Parser(query.sql).parse());const auto& s=binding.statement;
    if(parameters.size()<s.parameters)throw Error{87,"MSI SQL 缺少參數"};Result result;
    if(s.action==Action::Select){
        auto rows=joined(binding,parameters);
        const auto ids=string_indices(database);
        const auto less=[&](const Cell& a,const Cell& b){if(const auto* text=std::get_if<std::u16string>(&a))if(const auto* other=std::get_if<std::u16string>(&b))return ids.at(*text)<ids.at(*other);return a<b;};
        std::stable_sort(rows.begin(),rows.end(),[&](const auto& a,const auto& b){for(const auto index:binding.ordered){if(a.fields[index]!=b.fields[index])return less(a.fields[index],b.fields[index]);}return false;});
        for(const auto index:binding.selected)result.columns.push_back(binding.columns[index]);std::set<std::vector<Cell>> unique;
        for(const auto& row:rows){Row projected;for(const auto index:binding.selected)projected.fields.push_back(row.fields[index]);if(!s.distinct || unique.insert(projected.fields).second)result.rows.push_back(std::move(projected));}
        return result;
    }
    // 全部語義與欄位檢查通過後才發布，失敗的多列更新不留下部分修改。
    auto db=database;const auto& name=s.tables[0];
    if(s.action==Action::Create){
        Table table{name,s.definitions,{}};table.persistent=std::any_of(table.columns.begin(),table.columns.end(),[](const auto& c){return !(c.type&temporary);});table.holds=s.hold?1:0;db.tables.emplace(name,std::move(table));
    }else if(s.action==Action::Drop)db.tables.erase(name);
    else if(s.action==Action::Hold){auto& holds=db.tables.at(name).holds;if(holds==UINT32_MAX)failure();++holds;}
    else if(s.action==Action::Free)free_temporary(db,name);
    else if(s.action==Action::Add){auto& table=db.tables.at(name);table.columns.push_back(s.definitions[0]);for(auto& row:table.rows)row.fields.push_back({});if(s.hold){if(table.holds==UINT32_MAX)failure();++table.holds;}validate(table);}
    else if(s.action==Action::Insert){
        auto& table=db.tables.at(name);Row row;row.fields.resize(table.columns.size());row.temporary=s.temporary_row;
        for(std::size_t i=0;i<binding.selected.size();++i){const auto column=binding.selected[i];row.fields[column]=convert(table.columns[column],value(s.values[i],{},parameters));}
        table.rows.push_back(std::move(row));validate(table);
    }else {
        const auto rows=joined(binding,parameters);
        if(s.action==Action::Delete){auto& table=db.tables.at(name);std::set<std::size_t,std::greater<>> indices;for(const auto& row:rows)indices.insert(row.indices[0]);for(const auto index:indices)table.rows.erase(table.rows.begin()+static_cast<std::ptrdiff_t>(index));}
        else if(s.action==Action::Update){
            for(const auto& row:rows)for(std::size_t i=0;i<binding.selected.size();++i){
                const auto index=binding.selected[i];const auto table=static_cast<std::size_t>(std::upper_bound(binding.table_bases.begin(),binding.table_bases.end(),index)-binding.table_bases.begin()-1);
                auto& target=db.tables.at(s.tables[table]);const auto column=index-binding.table_bases[table];target.rows[row.indices[table]].fields[column]=convert(target.columns[column],value(s.values[i],row.fields,parameters));
            }
            for(const auto& table:s.tables)validate(db.tables.at(table));
        }
    }
    db.string_ids=string_indices(db);database=std::move(db);return result;
}
}
