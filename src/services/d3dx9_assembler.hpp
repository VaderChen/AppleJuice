#pragma once
#include "x64arm/windows_graphics.hpp"
#include <algorithm>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string_view>

namespace x64arm::d3dx9 {
// 直接產生 D3D9 token stream；相對位址與 SM1／SM2 長度規則在此共用。
class Assembler {
    struct Op {unsigned code,dest,sources;};
    unsigned major{},minor{},line{};bool pixel{};
    std::vector<std::uint32_t> words;
    [[noreturn]] static void fail(const std::string& what){throw std::invalid_argument(what);}
    static std::string trim(std::string s){const auto a=s.find_first_not_of(" \t\r\n"),b=s.find_last_not_of(" \t\r\n");return a==s.npos?std::string{}:s.substr(a,b-a+1);}
    static unsigned number(std::string_view s){unsigned n{};const auto r=std::from_chars(s.data(),s.data()+s.size(),n);if(r.ec!=std::errc{} || r.ptr!=s.data()+s.size())fail("無效整數："+std::string(s));return n;}
    static unsigned component(char c){const auto at=std::string_view("xyzw").find(c);if(at!=std::string_view::npos)return static_cast<unsigned>(at);const auto color=std::string_view("rgba").find(c);if(color!=std::string_view::npos)return static_cast<unsigned>(color);fail("無效分量");}
    static std::uint32_t regtype(unsigned type){return ((type&7U)<<28)|((type&24U)<<8);}
    void operand(std::string text,bool dest,std::vector<std::uint32_t>& out,unsigned modifier=0,unsigned shift=0){
        text.erase(std::remove_if(text.begin(),text.end(),[](unsigned char c){return std::isspace(c);}),text.end());
        if(text.empty())fail("缺少暫存器");
        unsigned source_modifier=0;
        if(!dest){
            if(text.starts_with("1-")){source_modifier=6;text.erase(0,2);}
            else if(text[0]=='-'){source_modifier=1;text.erase(0,1);}
            else if(text[0]=='!'){source_modifier=13;text.erase(0,1);}
            if(text.starts_with("abs(" ) && text.ends_with(')')){source_modifier=source_modifier==1?12:11;text=text.substr(4,text.size()-5);}
            else if(text.starts_with('|') && text.ends_with('|')){source_modifier=source_modifier==1?12:11;text=text.substr(1,text.size()-2);}
        }
        const auto bracket=text.find('[');const auto close=text.find(']');
        std::string relative;
        if(bracket!=text.npos){if(close==text.npos || close<bracket)fail("相對位址括號未配對");relative=text.substr(bracket+1,close-bracket-1);text.erase(bracket,close-bracket+1);}
        const auto dot=text.find('.');std::string swizzle=dot==text.npos?"":text.substr(dot+1);if(dot!=text.npos)text.resize(dot);
        if(const auto suffix=text.find('_');suffix!=text.npos){
            const auto name=text.substr(suffix);text.resize(suffix);
            const std::map<std::string,unsigned> modifiers{{"_bias",2},{"_bx2",4},{"_x2",7},{"_dz",9},{"_dw",10},{"_abs",11}};
            auto it=modifiers.find(name);if(dest || it==modifiers.end())fail("無效來源修飾："+name);
            const bool neg=source_modifier==1;if(source_modifier && !neg)fail("來源修飾衝突");source_modifier=it->second;
            if(neg){if(source_modifier==2 || source_modifier==4 || source_modifier==7 || source_modifier==11)++source_modifier;else fail("不合法負號修飾");}
        }
        unsigned type{},index{};std::string digits;
        if(text=="opos"){type=4;index=0;}
        else if(text=="ofog"){type=4;index=1;}
        else if(text=="opts"){type=4;index=2;}
        else if(text=="odepth"){type=9;}
        else if(text=="al"){type=15;}
        else if(text=="vpos"){type=17;}
        else if(text=="vface"){type=17;index=1;}
        else{
            unsigned prefix=1;
            if(text.starts_with("od")){type=5;prefix=2;}
            else if(text.starts_with("ot")){type=6;prefix=2;}
            else if(text.starts_with("oc")){type=8;prefix=2;}
            else switch(text[0]){case 'r':type=0;break;case 'v':type=1;break;case 'c':type=2;break;case 'a':type=3;break;case 't':type=3;break;case 'o':type=6;break;case 'i':type=7;break;case 's':type=10;break;case 'b':type=14;break;case 'l':type=18;break;case 'p':type=19;break;default:fail("未知暫存器："+text);}
            digits=text.substr(prefix);index=digits.empty() && !relative.empty()?0:number(digits);
        }
        std::uint32_t address=0;
        if(!relative.empty()){
            const auto plus=relative.find('+'),minus=relative.find('-',1);const auto split=plus!=relative.npos?plus:minus;
            if(split!=relative.npos){const auto offset=number(relative.substr(split+1));if(plus!=relative.npos)index+=offset;else{if(offset>index)fail("相對位址偏移為負");index-=offset;}relative.resize(split);}
            if(relative!="a0.x" && relative!="a0.y" && relative!="a0.z" && relative!="a0.w" && relative!="al")fail("無效相對位址："+relative);
            const unsigned lane=relative=="al"?0:component(relative.back());address=0x80000000U|regtype(relative=="al"?15:3)|(lane*0x55U<<16);
            if(major==1 && (dest || type!=2 || relative!="a0.x"))fail("SM1 僅支援常數 a0.x 相對定址");
        }
        if(type==2 && index>=2048){const auto bank=index/2048;if(bank>3)fail("常數索引超出範圍");type=10+bank;index%=2048;}
        if(index>2047)fail("暫存器索引超出範圍");
        std::uint32_t token=0x80000000U|regtype(type)|index|(!relative.empty()?0x2000:0);
        if(dest){unsigned mask=swizzle.empty()?15:0;for(char c:swizzle){const auto bit=1U<<component(c);if(mask&bit)fail("目的分量重複");mask|=bit;}token|=(mask<<16)|(modifier<<20)|(shift<<24);}
        else{if(swizzle.size()>4)fail("來源分量過多");unsigned lanes=0;if(swizzle.empty())lanes=0xe4;else for(unsigned i=0;i<4;++i)lanes|=component(swizzle[std::min<std::size_t>(i,swizzle.size()-1)])<<(2*i);token|=(lanes<<16)|(source_modifier<<24);}
        out.push_back(token);if(!relative.empty() && major>=2)out.push_back(address);
    }
    void instruction(std::string text){
        text=trim(text);if(text.empty() || text[0]=='#')return;
        std::transform(text.begin(),text.end(),text.begin(),[](unsigned char c){return std::tolower(c);});
        if(words.empty()){
            if(text.size()!=6 || (text.substr(0,2)!="vs" && text.substr(0,2)!="ps") || (text[2]!='.' && text[2]!='_') || text[4]!=text[2])fail("缺少著色器版本宣告");
            pixel=text[0]=='p';major=number(text.substr(3,1));minor=number(text.substr(5,1));
            if(major<1 || major>3 || (major>1 && minor!=0) || (major==1 && (pixel?(minor<1 || minor>4):minor!=1)))fail("不支援的著色器版本");
            words.push_back((pixel?0xffff0000U:0xfffe0000U)|(major<<8)|minor);return;
        }
        unsigned control=0,destmod=0,shift=0;bool coissue=false;
        if(text.starts_with('+')){coissue=true;text=trim(text.substr(1));}
        const auto space=text.find_first_of(" \t");std::string name=text.substr(0,space),args=space==text.npos?"":trim(text.substr(space));
        std::vector<std::string> operands;std::size_t begin=0;int depth=0;
        for(std::size_t i=0;i<=args.size();++i){if(i<args.size()){if(args[i]=='[' || args[i]=='(')++depth;if(args[i]==']' || args[i]==')')--depth;}
            if(i==args.size() || (args[i]==',' && !depth)){if(i>begin)operands.push_back(trim(args.substr(begin,i-begin)));else if(!args.empty())fail("空白運算元");begin=i+1;}}
        if(depth)fail("運算元括號未配對");
        std::vector<std::uint32_t> encoded;unsigned opcode{};
        if(name=="dcl" || name.starts_with("dcl_")){
            if(operands.size()!=1)fail("dcl 需要一個暫存器");
            auto semantic=name=="dcl"?"":name.substr(4);unsigned usage=0,usageindex=0,texture=0;
            if(semantic.ends_with("_centroid")){destmod|=4;semantic.resize(semantic.size()-9);}
            if(semantic=="2d")texture=2;else if(semantic=="cube")texture=3;else if(semantic=="volume")texture=4;
            else if(!semantic.empty()){
                const auto at=semantic.find_first_of("0123456789");if(at!=semantic.npos){usageindex=number(semantic.substr(at));semantic.resize(at);}
                const std::vector<std::string> usages{"position","blendweight","blendindices","normal","psize","texcoord","tangent","binormal","tessfactor","positiont","color","fog","depth","sample"};
                const auto it=std::find(usages.begin(),usages.end(),semantic);if(it==usages.end() || usageindex>15)fail("無效宣告語義");usage=static_cast<unsigned>(it-usages.begin());
            }
            encoded.push_back(0x80000000U|usage|(usageindex<<16)|(texture<<27));operand(operands[0],true,encoded,destmod);opcode=31;
        }else if(name=="def" || name=="defi" || name=="defb"){
            const unsigned count=name=="defb"?1:4;if(operands.size()!=count+1)fail("常數宣告數量不符");
            operand(operands[0],true,encoded);opcode=name=="def"?81:name=="defi"?48:47;
            const unsigned expected=name=="def"?2:name=="defi"?7:14;if(((encoded[0]>>28)&7U)!= (expected&7U) || ((encoded[0]>>8)&24U)!=(expected&24U))fail("常數暫存器類型不符");
            for(unsigned i=1;i<=count;++i){char* end=nullptr;const auto& value=operands[i];
                if(name=="defb"){if(value!="true" && value!="false" && value!="0" && value!="1")fail("無效布林常數");encoded.push_back(value=="true" || value=="1");}
                else if(name=="defi"){const auto v=std::strtoll(value.c_str(),&end,0);if(end!=value.c_str()+value.size() || v<INT32_MIN || v>INT32_MAX)fail("無效整數常數");encoded.push_back(static_cast<std::uint32_t>(v));}
                else{const float v=std::strtof(value.c_str(),&end);if(end!=value.c_str()+value.size())fail("無效浮點常數");encoded.push_back(std::bit_cast<std::uint32_t>(v));}}
        }else{
            const auto suffix=name.find('_');if(suffix!=name.npos){auto modifiers=name.substr(suffix+1);name.resize(suffix);std::istringstream parts(modifiers);std::string part;
                while(std::getline(parts,part,'_')){if(part=="sat")destmod|=1;else if(part=="pp")destmod|=2;else if(part=="centroid")destmod|=4;
                    else if(part=="x2")shift=1;else if(part=="x4")shift=2;else if(part=="x8")shift=3;else if(part=="d2")shift=15;else if(part=="d4")shift=14;else if(part=="d8")shift=13;
                    else{const std::map<std::string,unsigned> cmp{{"gt",1},{"eq",2},{"ge",3},{"lt",4},{"ne",5},{"le",6}};auto it=cmp.find(part);if(it==cmp.end())fail("未知指令修飾："+part);control=it->second;}}}
            static const std::map<std::string,Op> operations{
                {"nop",{0,0,0}},{"mov",{1,1,1}},{"add",{2,1,2}},{"sub",{3,1,2}},{"mad",{4,1,3}},{"mul",{5,1,2}},
                {"rcp",{6,1,1}},{"rsq",{7,1,1}},{"dp3",{8,1,2}},{"dp4",{9,1,2}},{"min",{10,1,2}},{"max",{11,1,2}},
                {"slt",{12,1,2}},{"sge",{13,1,2}},{"exp",{14,1,1}},{"log",{15,1,1}},{"lit",{16,1,1}},{"dst",{17,1,2}},
                {"lrp",{18,1,3}},{"frc",{19,1,1}},{"m4x4",{20,1,2}},{"m4x3",{21,1,2}},{"m3x4",{22,1,2}},{"m3x3",{23,1,2}},{"m3x2",{24,1,2}},
                {"call",{25,0,1}},{"callnz",{26,0,2}},{"loop",{27,0,2}},{"ret",{28,0,0}},{"endloop",{29,0,0}},{"label",{30,0,1}},
                {"pow",{32,1,2}},{"crs",{33,1,2}},{"sgn",{34,1,3}},{"abs",{35,1,1}},{"nrm",{36,1,1}},{"sincos",{37,1,3}},
                {"rep",{38,0,1}},{"endrep",{39,0,0}},{"if",{40,0,1}},{"else",{42,0,0}},{"endif",{43,0,0}},{"break",{44,0,0}},{"mova",{46,1,1}},
                {"texcoord",{64,1,0}},{"texcrd",{64,1,1}},{"texkill",{65,1,0}},{"tex",{66,1,0}},{"texld",{66,1,2}},{"texldp",{66,1,2}},{"texldb",{66,1,2}},
                {"texbem",{67,1,1}},{"texbeml",{68,1,1}},{"texreg2ar",{69,1,1}},{"texreg2gb",{70,1,1}},
                {"texm3x2pad",{71,1,1}},{"texm3x2tex",{72,1,1}},{"texm3x3pad",{73,1,1}},{"texm3x3tex",{74,1,1}},{"texm3x3spec",{76,1,2}},{"texm3x3vspec",{77,1,1}},
                {"expp",{78,1,1}},{"logp",{79,1,1}},{"cnd",{80,1,3}},{"texreg2rgb",{82,1,1}},{"texdp3tex",{83,1,1}},{"texm3x2depth",{84,1,1}},{"texdp3",{85,1,1}},{"texm3x3",{86,1,1}},{"texdepth",{87,1,0}},
                {"cmp",{88,1,3}},{"bem",{89,1,2}},{"dp2add",{90,1,3}},{"dsx",{91,1,1}},{"dsy",{92,1,1}},{"texldd",{93,1,4}},{"setp",{94,1,2}},{"texldl",{95,1,2}},{"breakp",{96,0,1}},{"phase",{0xfffd,0,0}}};
            const auto it=operations.find(name);if(it==operations.end())fail("未知指令："+name);auto op=it->second;
            if(control){if(name=="if"){op.code=41;op.sources=2;}else if(name=="break"){op.code=45;op.sources=2;}else if(name!="setp")fail("比較修飾僅適用於條件指令");}
            if(name=="texldp")control=1;if(name=="texldb")control=2;
            if(major==1 && name=="texld")op.sources=minor==4?1:0;
            if(major>=3 && (name=="sincos" || name=="sgn"))op.sources=1;
            if(operands.size()!=op.dest+op.sources)fail(name+" 運算元數量不符");
            if(!op.dest && (destmod || shift))fail("無目的暫存器的指令不可使用目的修飾");
            for(unsigned i=0;i<operands.size();++i)operand(operands[i],i<op.dest,encoded,destmod,shift);opcode=op.code;
        }
        if(encoded.size()>15)fail("指令過長");
        words.push_back(opcode|(control<<16)|(major>=2?static_cast<unsigned>(encoded.size())<<24:0)|(coissue?0x40000000U:0));words.insert(words.end(),encoded.begin(),encoded.end());
    }
public:
    GraphicsCompilation assemble(std::span<const std::uint8_t> source,const std::string& filename){
        GraphicsCompilation result;
        try{
            std::string text(reinterpret_cast<const char*>(source.data()),source.size());std::istringstream lines(text);std::string current;bool block=false;
            while(std::getline(lines,current)){++line;std::string cleaned;
                for(std::size_t i=0;i<current.size();++i){if(block){if(current[i]=='*' && i+1<current.size() && current[i+1]=='/'){block=false;++i;}continue;}
                    if(current[i]==';' || (current[i]=='/' && i+1<current.size() && current[i+1]=='/'))break;
                    if(current[i]=='/' && i+1<current.size() && current[i+1]=='*'){block=true;++i;cleaned+=' ';continue;}cleaned+=current[i];}
                instruction(cleaned);
            }
            if(block)fail("未結束的註解");if(words.empty())fail("缺少著色器");words.push_back(0x0000ffffU);
            result.bytecode.resize(words.size()*4);std::memcpy(result.bytecode.data(),words.data(),result.bytecode.size());
        }catch(const std::invalid_argument& error){result.messages=filename+":"+std::to_string(line)+": "+error.what()+"\n";}
        return result;
    }
};
}
