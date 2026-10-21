#pragma once
// X-file 自訂模板與資料階層；文字、二進位及 MSZIP 共用 tokenizer。
namespace x64arm::d3d9_xfile {
using FileGuid=std::array<std::uint8_t,16>;
struct Member {std::string type,name;std::vector<Token> dimensions;};
struct Template {FileGuid id{};std::vector<Member> members;bool open{};std::set<std::string> children;};
using Templates=std::map<std::string,Template>;
struct DataNode {std::string name,type,reference;FileGuid id{},type_id{};std::vector<std::uint8_t> bytes;std::vector<unsigned> children;unsigned target{UINT32_MAX};std::uint64_t guest{};};
struct Document {Templates templates;std::vector<DataNode> nodes;std::vector<unsigned> roots;};
class DocumentParser {
    std::vector<Token> tokens_;std::size_t cursor_{};Document document_;
    static void check(bool condition){if(!condition)throw std::invalid_argument("X-file 模板／資料格式不符");}
    bool is(std::string_view value)const{return cursor_<tokens_.size() && tokens_[cursor_].text==value;}
    Token take(){check(cursor_<tokens_.size());return tokens_[cursor_++];}
    void expect(std::string_view value){check(is(value));++cursor_;}
    void separators(){while(is(";") || is(","))++cursor_;}
    std::string name(){auto token=take();check(!token.numeric && !token.uuid && !token.text.empty());return token.text;}
    void definition(){
        expect("template");const auto label=name();expect("{");const auto uuid=take();check(uuid.uuid);Template value;value.id=uuid.guid;
        while(!is("}")){
            if(is("[")){++cursor_;while(!is("]")){const auto token=take();if(token.text=="..." || token.text==".")value.open=true;else if(token.text!="," && !token.uuid)value.children.insert(token.text);}expect("]");separators();continue;}
            Member member;const bool array=is("array");if(array)++cursor_;member.type=name();if(!is(";") && !is("["))member.name=name();
            if(array){check(is("["));while(is("[")){++cursor_;member.dimensions.push_back(take());expect("]");}}
            expect(";");value.members.push_back(std::move(member));
        }expect("}");document_.templates[label]=std::move(value);
    }
    template<class T> static void append(DataNode& data,T value){check(data.bytes.size()<=512U*1024U*1024U-sizeof(T));const auto at=data.bytes.size();data.bytes.resize(at+sizeof(T));std::memcpy(data.bytes.data()+at,&value,sizeof(T));}
    double scalar(){separators();const auto token=take();check(token.numeric && std::isfinite(token.number));return token.number;}
    void primitive(DataNode& data,const std::string& type,std::map<std::string,std::uint64_t>& integers,const std::string& field,unsigned depth){
        check(depth<128);separators();
        if(type=="STRING" || type=="LPSTR"){
            const auto value=take();check(!value.numeric && !value.uuid);data.bytes.insert(data.bytes.end(),value.text.begin(),value.text.end());data.bytes.push_back(0);return;
        }
        if(type=="FLOAT" || type=="float"){append(data,static_cast<float>(scalar()));return;}
        if(type=="DOUBLE" || type=="double"){append(data,scalar());return;}
        const std::map<std::string,std::pair<unsigned,bool>> integral{{"WORD",{2,false}},{"DWORD",{4,false}},{"CHAR",{1,true}},{"UCHAR",{1,false}},{"BYTE",{1,false}},{"SWORD",{2,true}},{"SDWORD",{4,true}}};
        if(const auto it=integral.find(type);it!=integral.end()){
            const auto value=scalar();const auto [bytes,signed_value]=it->second;const auto bits=bytes*8;const double low=signed_value?-std::ldexp(1.,static_cast<int>(bits)-1):0,high=std::ldexp(1.,static_cast<int>(bits)-(signed_value?1:0))-1;
            check(value>=low && value<=high && std::floor(value)==value);const auto number=static_cast<std::uint64_t>(static_cast<std::int64_t>(value));integers[field]=number;
            for(unsigned i=0;i<bytes;++i)append(data,static_cast<std::uint8_t>(number>>(i*8)));return;
        }
        check(document_.templates.contains(type));members(data,document_.templates.at(type),depth+1);
    }
    void members(DataNode& data,const Template& value,unsigned depth){
        std::map<std::string,std::uint64_t> integers;
        for(const auto& member:value.members){std::uint64_t count=1;
            for(const auto& dimension:member.dimensions){std::uint64_t size=0;if(dimension.numeric){check(dimension.number>=0 && dimension.number<=16777216 && std::floor(dimension.number)==dimension.number);size=static_cast<std::uint64_t>(dimension.number);}else{check(integers.contains(dimension.text));size=integers.at(dimension.text);}check(size<=16777216 && (!size || count<=16777216/size));count*=size;}
            for(std::uint64_t i=0;i<count;++i)primitive(data,member.type,integers,member.name,depth);
            separators();
        }
    }
    unsigned object(unsigned depth){
        check(depth<128 && document_.nodes.size()<1048576);DataNode data;
        if(is("{")){++cursor_;const auto value=take();if(value.uuid){data.id=value.guid;data.reference="@guid";}else data.reference=value.text;expect("}");}
        else{
            data.type=name();check(document_.templates.contains(data.type));const auto definition=document_.templates.at(data.type);data.type_id=definition.id;
            if(!is("{"))data.name=name();expect("{");if(cursor_<tokens_.size() && tokens_[cursor_].uuid)data.id=take().guid;
            members(data,definition,depth);separators();while(!is("}")){const auto child=object(depth+1);const auto& node=document_.nodes[child];check(!node.reference.empty() || definition.open || definition.children.contains(node.type));data.children.push_back(child);separators();}expect("}");
        }
        const auto index=static_cast<unsigned>(document_.nodes.size());document_.nodes.push_back(std::move(data));return index;
    }
public:
    DocumentParser(std::span<const std::uint8_t> bytes,const Templates& templates){const auto expanded=decompress(bytes);tokens_=Parser(expanded).token_stream();document_.templates=templates;}
    Document parse(){
        while(cursor_<tokens_.size()){separators();if(cursor_==tokens_.size())break;if(is("template"))definition();else document_.roots.push_back(object(0));}
        for(auto& node:document_.nodes)if(!node.reference.empty()){
            for(unsigned i=0;i<document_.nodes.size();++i){const auto& target=document_.nodes[i];if(target.reference.empty() && (node.reference=="@guid"?target.id==node.id:target.name==node.reference)){node.target=i;node.type_id=target.type_id;node.type=target.type;break;}}check(node.target!=UINT32_MAX);
        }
        return std::move(document_);
    }
};
}
