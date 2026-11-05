#pragma once
#include "x64arm/windows_abi.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace x64arm::msvcp90_string {
inline constexpr std::uint64_t image_offset=0xf0000;
// VC9 的字串含一個 allocator 指標、16-byte SSO、size 與 capacity。
// 與 VC10／VC14 分開描述 ABI，操作語義共用；名稱只在初始化時展開。
enum class Op { Construct,Destroy,Assign,Append,Insert,Replace,Erase,Reserve,Resize,
    Clear,Size,Capacity,Empty,Maximum,Data,At,Index,Begin,End,RBegin,REnd,
    Compare,Find,RFind,FirstOf,LastOf,FirstNotOf,LastNotOf,Substr,Copy,Swap,
    Tidy,Grow,Eos,Inside,Concat,Equal,NotEqual,Less,Greater,LessEqual,GreaterEqual,
    EraseIterator,InsertIterator,InsertRange,Allocator,CopySafe,CopyCapacity,CharacterAssign };
enum class Input { None,Text,Span,String,Substring,Fill,Character,Range,Iterators };
struct Binding {
    std::string name;
    Op op{};
    Input input{};
    unsigned element{},arguments{},wide{};
    bool member{},x86{},iterator_range{},substring_lhs{},text_lhs{};
};
inline const std::vector<Binding>& bindings(){
    static const auto value=[] {
        std::vector<Binding> out;
        for(const bool x86:{true,false})for(const std::string c:{"D","_W","G"}){
            const unsigned element=c=="D"?1:2;
            const std::string stem="?$basic_string@"+c+"U?$char_traits@"+c+"@std@@V?$allocator@"+c+"@2@@std@@";
            const std::string m=x86?"QAE":"QEAA",k=x86?"QBE":"QEBA",priv=x86?"IAE":"IEAA",n=x86?"I":"_K";
            const std::string text=(x86?"PB":"PEB")+c,ptr=(x86?"PA":"PEA")+c;
            const std::string ref=x86?"AAV12@":"AEAV12@",copy=x86?"ABV12@":"AEBV12@";
            const std::string iter="V?$_String_const_iterator@"+c+"U?$char_traits@"+c+"@std@@V?$allocator@"+c+"@2@@2@";
            const auto add=[&](std::string name,Op op,Input input,unsigned args,bool member=true,unsigned wide=0,bool ir=false,bool lhs=false,bool tl=false){
                out.push_back({std::move(name),op,input,element,args+(!x86 && member?1U:0U),x86?wide:0U,member,x86,ir,lhs,tl});
            };
            const auto method=[&](std::string name,std::string abi,Op op,Input input,unsigned args,unsigned wide=0,bool ir=false,bool lhs=false){add("?"+name+"@"+stem+abi,op,input,args,true,wide,ir,lhs);};
            for(const auto& shape:std::vector<std::pair<std::string,Input>>{{"X",Input::None},{text,Input::Text},{text+n,Input::Span},{x86?"ABV01@":"AEBV01@",Input::String},{n+c,Input::Fill}}){
                const unsigned count=shape.second==Input::None?0:shape.second==Input::Span || shape.second==Input::Fill?2:1;
                add("??0"+stem+m+"@"+shape.first+(shape.second==Input::None?"Z":"@Z"),Op::Construct,shape.second,count);
            }
            const std::string allocator=(x86?"ABV?$allocator@":"AEBV?$allocator@")+c+"@1@";
            const std::string ctor_copy=x86?"ABV01@II":"AEBV01@_K1";
            add("??0"+stem+m+"@"+ctor_copy+"@Z",Op::Construct,Input::Substring,3);
            add("??0"+stem+m+"@"+ctor_copy+allocator+"@Z",Op::Construct,Input::Substring,4);
            add("??0"+stem+m+"@"+allocator+"@Z",Op::Construct,Input::None,1);
            add("??0"+stem+m+"@"+n+c+allocator+"@Z",Op::Construct,Input::Fill,3);
            add("??0"+stem+m+"@"+text+"0@Z",Op::Construct,Input::Range,2);
            add("??0"+stem+m+"@"+text+allocator+"@Z",Op::Construct,Input::Text,2);
            add("??0"+stem+m+"@"+text+n+allocator+"@Z",Op::Construct,Input::Span,3);
            add("??0"+stem+m+"@"+iter.substr(0,iter.size()-2)+"1@0@Z",Op::Construct,Input::Iterators,2,true,3);
            add("??1"+stem+m+"@XZ",Op::Destroy,Input::None,0);
            for(const auto& shape:std::vector<std::pair<std::string,Input>>{{text,Input::Text},{x86?"ABV01@":"AEBV01@",Input::String},{c,Input::Character}})
                add("??4"+stem+m+(x86?"AAV01@":"AEAV01@")+shape.first+"@Z",Op::Assign,shape.second,1);
            for(const auto& shape:std::vector<std::pair<std::string,Input>>{{text,Input::Text},{x86?"ABV01@":"AEBV01@",Input::String},{c,Input::Character}})
                add("??Y"+stem+m+(x86?"AAV01@":"AEAV01@")+shape.first+"@Z",Op::Append,shape.second,1);
            for(const auto& operation:std::vector<std::pair<std::string,Op>>{{"assign",Op::Assign},{"append",Op::Append},{"insert",Op::Insert},{"replace",Op::Replace}}){
                const unsigned prefix=operation.second==Op::Insert?1:operation.second==Op::Replace?2:0;
                std::string indices;for(unsigned i=0;i<prefix;++i)indices+=i && !x86?"0":n;
                for(const auto& shape:std::vector<std::pair<std::string,Input>>{{text,Input::Text},{text+(x86?n:prefix?"0":n),Input::Span},{copy,Input::String},{copy+(x86?"II":prefix?"00":"_K1"),Input::Substring},{(x86?n:prefix?"0":n)+c,Input::Fill}}){
                    const unsigned count=shape.second==Input::Substring?3:shape.second==Input::Span || shape.second==Input::Fill?2:1;
                    method(operation.first,m+ref+indices+shape.first+"@Z",operation.second,shape.second,prefix+count);
                }
                if(prefix==0){method(operation.first,m+ref+text+"0@Z",operation.second,Input::Range,2);method(operation.first,m+ref+iter+"0@Z",operation.second,Input::Iterators,2,3);}
            }
            // 安全迭代器是 {container,position}；x86 值傳遞 8 bytes，x64 間接傳遞。
            method("replace",m+ref+iter+"0"+copy+"@Z",Op::Replace,Input::String,3,3,true);
            method("replace",m+ref+iter+"0"+text+"@Z",Op::Replace,Input::Text,3,3,true);
            method("replace",m+ref+iter+"0"+text+n+"@Z",Op::Replace,Input::Span,4,3,true);
            method("replace",m+ref+iter+"0"+n+c+"@Z",Op::Replace,Input::Fill,4,3,true);
            method("replace",m+ref+iter+"000@Z",Op::Replace,Input::Iterators,4,15,true);
            method("replace",m+ref+iter+"0"+text+"1@Z",Op::Replace,Input::Range,4,3,true);
            const std::string returned_iterator="?AV?$_String_iterator@"+c+"U?$char_traits@"+c+"@std@@V?$allocator@"+c+"@2@@2@";
            method("erase",m+returned_iterator+iter+"@Z",Op::EraseIterator,Input::None,2,2);
            method("erase",m+returned_iterator+iter+"0@Z",Op::EraseIterator,Input::Iterators,3,6);
            method("insert",m+returned_iterator+iter+"@Z",Op::InsertIterator,Input::None,2,2);
            method("insert",m+returned_iterator+iter+c+"@Z",Op::InsertIterator,Input::Character,3,2);
            method("insert",m+"X"+iter+"00@Z",Op::InsertRange,Input::Iterators,3,7);
            method("insert",m+"X"+iter+n+c+"@Z",Op::InsertRange,Input::Fill,3,1);
            method("insert",m+"X"+iter+text+"1@Z",Op::InsertRange,Input::Range,3,1);
            method("erase",m+ref+n+(x86?"I":"0")+"@Z",Op::Erase,Input::None,2);
            for(const auto& p:std::vector<std::pair<std::string,Op>>{{"reserve",Op::Reserve},{"resize",Op::Resize}}){
                method(p.first,m+"X"+n+"@Z",p.second,Input::None,1);
                if(p.second==Op::Resize)method(p.first,m+"X"+n+c+"@Z",p.second,Input::Character,2);
            }
            method("clear",m+"XXZ",Op::Clear,Input::None,0);
            method("push_back",m+"X"+c+"@Z",Op::Append,Input::Character,1);
            method("swap",m+"X"+(x86?"AAV12@":"AEAV12@")+"@Z",Op::Swap,Input::None,1);
            for(const auto& p:std::vector<std::pair<std::string,Op>>{{"size",Op::Size},{"length",Op::Size},{"capacity",Op::Capacity},{"max_size",Op::Maximum}})
                method(p.first,k+n+"XZ",p.second,Input::None,0);
            method("empty",k+"_NXZ",Op::Empty,Input::None,0);
            for(const auto name:{"c_str","data"})method(name,k+text+"XZ",Op::Data,Input::None,0);
            for(const bool constant:{false,true}){
                const auto access=constant?k:m,reference=(x86?(constant?"AB":"AA"):(constant?"AEB":"AEA"))+c;
                method("at",access+reference+n+"@Z",Op::At,Input::None,1);
                add("??A"+stem+access+reference+n+"@Z",Op::Index,Input::None,1);
                add("??A"+stem+access+reference+"U_Size_type_nosscl@01@@Z",Op::Index,Input::None,1);
                for(const auto& p:std::vector<std::pair<std::string,Op>>{{"begin",Op::Begin},{"end",Op::End},{"rbegin",Op::RBegin},{"rend",Op::REnd}}){
                    std::string it="V?$_String_"+std::string(constant?"const_iterator@":"iterator@")+c+"U?$char_traits@"+c+"@std@@V?$allocator@"+c+"@2@@";
                    if(p.second==Op::RBegin || p.second==Op::REnd)it="V?$reverse_iterator@"+it+"std@@@2@";else it+="2@";
                    method(p.first,access+"?A"+it+"XZ",p.second,Input::None,1);
                }
            }
            for(const auto& shape:std::vector<std::pair<std::string,Input>>{{text,Input::Text},{copy,Input::String}})
                method("compare",k+"H"+shape.first+"@Z",Op::Compare,shape.second,1);
            for(const auto& shape:std::vector<std::pair<std::string,Input>>{{text,Input::Text},{text+(x86?"I":"0"),Input::Span},{copy,Input::String},{copy+(x86?"II":"00"),Input::Substring}})
                method("compare",k+"H"+n+(x86?"I":"0")+shape.first+"@Z",Op::Compare,shape.second,shape.second==Input::Substring?5:shape.second==Input::Span?4:3,0,false,true);
            for(const auto& p:std::vector<std::pair<std::string,Op>>{{"find",Op::Find},{"rfind",Op::RFind},{"find_first_of",Op::FirstOf},{"find_last_of",Op::LastOf},{"find_first_not_of",Op::FirstNotOf},{"find_last_not_of",Op::LastNotOf}}){
                method(p.first,k+n+text+n+"@Z",p.second,Input::Text,2);
                method(p.first,k+n+text+n+(x86?"I":"1")+"@Z",p.second,Input::Span,3);
                method(p.first,k+n+copy+n+"@Z",p.second,Input::String,2);
                method(p.first,k+n+c+n+"@Z",p.second,Input::Character,2);
            }
            method("substr",k+"?AV12@"+n+(x86?"I":"0")+"@Z",Op::Substr,Input::None,3);
            method("copy",k+n+ptr+n+(x86?"I":"1")+"@Z",Op::Copy,Input::None,3);
            method("_Copy_s",k+n+ptr+n+(x86?"II":"11")+"@Z",Op::CopySafe,Input::None,4);
            method("get_allocator",k+"?AV?$allocator@"+c+"@2@XZ",Op::Allocator,Input::None,1);
            method("_Copy",priv+"X"+n+(x86?"I":"0")+"@Z",Op::CopyCapacity,Input::None,2);
            method("_Chassign",priv+"X"+n+(x86?"I":"0")+c+"@Z",Op::CharacterAssign,Input::None,3);
            method("_Myptr",priv+ptr+"XZ",Op::Data,Input::None,0);
            method("_Myptr",std::string(x86?"IBE":"IEBA")+text+"XZ",Op::Data,Input::None,0);
            method("_Tidy",priv+"X_N"+n+"@Z",Op::Tidy,Input::None,2);
            method("_Grow",priv+"_N"+n+"_N@Z",Op::Grow,Input::None,2);
            method("_Eos",priv+"X"+n+"@Z",Op::Eos,Input::None,1);
            method("_Inside",priv+"_N"+text+"@Z",Op::Inside,Input::None,1);
            const std::string global=c+"U?$char_traits@"+c+"@std@@V?$allocator@"+c+"@1@@std@@";
            const std::string object="V?$basic_string@"+c+"U?$char_traits@"+c+"@std@@V?$allocator@"+c+"@2@@0@";
            const std::string first=(x86?"AB":"AEB")+object;
            for(const auto& p:std::vector<std::pair<std::string,Op>>{{"8",Op::Equal},{"9",Op::NotEqual},{"M",Op::Less},{"O",Op::Greater},{"N",Op::LessEqual},{"P",Op::GreaterEqual}}){
                add("??$?"+p.first+global+"YA_N"+first+"0@Z",p.second,Input::String,2,false);
                add("??$?"+p.first+global+"YA_N"+first+text+"@Z",p.second,Input::Text,2,false);
                add("??$?"+p.first+global+"YA_N"+text+first+"@Z",p.second,Input::Text,2,false,0,false,false,true);
            }
            for(const auto& p:std::vector<std::pair<std::string,Input>>{{x86?"ABV10@0":"AEBV10@0",Input::String},{(x86?"ABV10@":"AEBV10@")+text,Input::Text},{(x86?"ABV10@":"AEBV10@")+c,Input::Character}})
                add("??$?H"+global+"YA?A"+object+p.first+"@Z",Op::Concat,p.second,3,false);
            add("??$?H"+global+"YA?A"+object+text+(x86?"ABV10@":"AEBV10@")+"@Z",Op::Concat,Input::Text,3,false,0,false,false,true);
            add("??$?H"+global+"YA?A"+object+c+(x86?"ABV10@":"AEBV10@")+"@Z",Op::Concat,Input::Character,3,false,0,false,false,true);
        }
        return out;
    }();return value;
}
inline const Binding* binding(std::string_view symbol){
    static const auto index=[] {std::unordered_map<std::string_view,const Binding*> m;for(const auto& e:bindings())m.emplace(e.name,&e);return m;}();
    const auto i=index.find(symbol);return i==index.end()?nullptr:i->second;
}
inline std::string native_name(const Binding& entry){
    std::string name;
    switch(entry.op){
    case Op::Size:name="size";break;case Op::Capacity:name="capacity";break;
    case Op::Empty:name="empty";break;case Op::Maximum:name="maximum";break;
    case Op::Data:name="data";break;case Op::Inside:name="inside";break;
    case Op::Begin:case Op::REnd:name="begin";break;case Op::End:case Op::RBegin:name="end";break;
    case Op::Compare:if(entry.substring_lhs)return {};name="compare";break;
    case Op::Find:name="find";break;case Op::RFind:name="rfind";break;
    case Op::FirstOf:name="first_of";break;case Op::LastOf:name="last_of";break;
    case Op::FirstNotOf:name="first_not_of";break;case Op::LastNotOf:name="last_not_of";break;
    case Op::Equal:name="equal";break;case Op::NotEqual:name="not_equal";break;
    case Op::Less:name="less";break;case Op::Greater:name="greater";break;
    case Op::LessEqual:name="less_equal";break;case Op::GreaterEqual:name="greater_equal";break;
    default:return {};
    }
    if(entry.input!=Input::None){
        if(entry.input==Input::Text)name+="_text";else if(entry.input==Input::Span)name+="_span";
        else if(entry.input==Input::String)name+="_string";else if(entry.input==Input::Character)name+="_character";else return {};
    }
    if(entry.text_lhs)name+="_lhs";
    return "guest_"+std::string(entry.element==1?"D_":"W_")+name;
}
inline std::vector<WindowsServiceDescriptor> descriptors(){
    std::vector<WindowsServiceDescriptor> out;for(const auto& e:bindings())out.push_back({"msvcp90.dll",e.name,e.arguments,e.wide,0,0,!e.member});return out;
}
}
