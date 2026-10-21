#include "x64arm/native_console.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <charconv>
#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace x64arm {
std::uint32_t console_field(const ConsoleRecord& value,unsigned at,unsigned size){
    std::uint32_t result=0;for(unsigned i=0;i<size;++i)result|=std::uint32_t{value.at(at+i)}<<(8*i);return result;
}
void set_console_field(ConsoleRecord& value,unsigned at,std::uint32_t field,unsigned size){
    for(unsigned i=0;i<size;++i)value.at(at+i)=static_cast<std::uint8_t>(field>>(8*i));
}
ConsoleRecord console_key(char16_t c,bool down,std::uint16_t key,std::uint16_t scan,std::uint32_t modifiers){
    ConsoleRecord value{};set_console_field(value,0,1,2);set_console_field(value,4,down?1:0);set_console_field(value,8,1,2);
    if(!key){if(c>=u'a' && c<=u'z')key=static_cast<std::uint16_t>(c-u'a'+u'A');else if((c>=u'A' && c<=u'Z') || (c>=u'0' && c<=u'9'))key=c;
        else if(c==u'\r' || c==u'\n')key=13;else if(c==u'\t')key=9;else if(c==u'\b' || c==127)key=8;else if(c==27)key=27;else if(c==u' ')key=32;}
    set_console_field(value,10,key,2);set_console_field(value,12,scan,2);set_console_field(value,14,c,2);set_console_field(value,16,modifiers);return value;
}
ConsoleBuffer::ConsoleBuffer(){cells.resize(static_cast<std::size_t>(columns)*rows);}
void ConsoleBuffer::resize(std::uint16_t width,std::uint16_t height,bool preserve_viewport){
    if(!width || !height || std::uint64_t{width}*height>1024U*1024U)return;
    std::vector<ConsoleCell> next(static_cast<std::size_t>(width)*height,ConsoleCell{u' ',attributes});
    for(unsigned row=0;row<std::min(rows,height);++row)
        std::copy_n(cells.begin()+row*columns,std::min(columns,width),next.begin()+row*width);
    cells=std::move(next);columns=width;rows=height;x=std::min<std::uint16_t>(x,width-1);y=std::min<std::uint16_t>(y,height-1);
    if(!preserve_viewport){left=top=0;view_columns=width;view_rows=height;}
    else {view_columns=std::min(view_columns,width);view_rows=std::min(view_rows,height);left=std::min<std::uint16_t>(left,width-view_columns);top=std::min<std::uint16_t>(top,height-view_rows);}
}
void ConsoleBuffer::resize_viewport(std::uint16_t width,std::uint16_t height){
    if(!width || !height)return;
    const bool following=!left && !top && view_columns==columns && view_rows==rows;
    if(following)resize(width,height);
    else {
        if(width>columns || height>rows)resize(std::max(width,columns),std::max(height,rows),true);
        view_columns=std::min(width,columns);view_rows=std::min(height,rows);
        left=std::min<std::uint16_t>(left,columns-view_columns);top=std::min<std::uint16_t>(top,rows-view_rows);
    }
}
void ConsoleBuffer::reveal_cursor(){
    if(x<left)left=x;else if(x>=left+view_columns)left=static_cast<std::uint16_t>(x-view_columns+1);
    if(y<top)top=y;else if(y>=top+view_rows)top=static_cast<std::uint16_t>(y-view_rows+1);
}
void ConsoleBuffer::push(ConsoleRecord value){
    if(records.size()+ansi_records.size()>=65536)return;
    const auto type=console_field(value,0,2);
    if((type==2 && !(input_mode&16)) || (type==4 && !(input_mode&8)))return;
    records.push_back(value);
}
void ConsoleBuffer::character(char16_t c){
    const auto newline=[&]{
        if(y+1<rows){++y;return;}
        std::move(cells.begin()+columns,cells.end(),cells.begin());
        std::fill(cells.end()-columns,cells.end(),ConsoleCell{u' ',attributes});
    };
    if((output_mode&1) && c<32){
        if(c==u'\r')x=0;else if(c==u'\n')newline();else if(c==u'\b'){if(x)--x;}
        else if(c==u'\t'){const auto end=std::min<unsigned>(columns,(x/8+1)*8);while(x<end){cells[static_cast<std::size_t>(y)*columns+x]={u' ',attributes};++x;}if(x>=columns){x=0;newline();}}
        return;
    }
    cells[static_cast<std::size_t>(y)*columns+x]={c,attributes};
    if(x+1<columns)++x;else if(output_mode&2){x=0;newline();}
}
void ConsoleBuffer::control_sequence(){
    if(escape.size()<3 || escape[1]!='[')return;
    std::vector<unsigned> args;auto text=std::string_view(escape).substr(2,escape.size()-3);bool private_mode=false;
    if(text.starts_with('?')){private_mode=true;text.remove_prefix(1);}
    while(!text.empty()){const auto separator=text.find(';');auto part=text.substr(0,separator);unsigned value=0;
        if(!part.empty()){const auto parsed=std::from_chars(part.data(),part.data()+part.size(),value);if(parsed.ec!=std::errc{})return;}
        args.push_back(value);if(separator==text.npos)break;text.remove_prefix(separator+1);
    }
    const auto argument=[&](unsigned index,unsigned fallback=1){return index<args.size() && args[index]?args[index]:fallback;};
    const auto final=escape.back();
    if(final=='H' || final=='f'){y=static_cast<std::uint16_t>(std::min<unsigned>(rows-1,argument(0)-1));x=static_cast<std::uint16_t>(std::min<unsigned>(columns-1,argument(1)-1));}
    else if(final=='A')y=static_cast<std::uint16_t>(y-std::min<unsigned>(y,argument(0)));
    else if(final=='B')y=static_cast<std::uint16_t>(std::min<unsigned>(rows-1,y+argument(0)));
    else if(final=='C')x=static_cast<std::uint16_t>(std::min<unsigned>(columns-1,x+argument(0)));
    else if(final=='D')x=static_cast<std::uint16_t>(x-std::min<unsigned>(x,argument(0)));
    else if(final=='G')x=static_cast<std::uint16_t>(std::min<unsigned>(columns-1,argument(0)-1));
    else if(final=='h' || final=='l'){if(private_mode && argument(0)==25)cursor_visible=final=='h';}
    else if(final=='J' || final=='K'){
        const auto mode=argument(0,0);auto first=static_cast<std::size_t>(y)*columns+x,last=cells.size();
        if(final=='K'){last=(static_cast<std::size_t>(y)+1)*columns;if(mode==1){last=first+1;first=static_cast<std::size_t>(y)*columns;}else if(mode==2)first=static_cast<std::size_t>(y)*columns;}
        else if(mode==1){last=first+1;first=0;}else if(mode==2 || mode==3)first=0;
        std::fill(cells.begin()+static_cast<std::ptrdiff_t>(first),cells.begin()+static_cast<std::ptrdiff_t>(last),ConsoleCell{u' ',attributes});
    }else if(final=='m'){
        if(args.empty())args.push_back(0);
        constexpr std::array<unsigned,8> colors{0,4,2,6,1,5,3,7};
        for(auto value:args){
            if(value==0)attributes=7;else if(value==1)attributes|=8;else if(value==22)attributes&=~8U;
            else if(value==7)attributes|=0x4000;else if(value==27)attributes&=~0x4000U;
            else if(value==4)attributes|=0x8000;else if(value==24)attributes&=~0x8000U;
            else if(value>=30 && value<=37)attributes=static_cast<std::uint16_t>((attributes&~7U)|colors[value-30]);
            else if(value>=40 && value<=47)attributes=static_cast<std::uint16_t>((attributes&~0x70U)|(colors[value-40]<<4));
            else if(value>=90 && value<=97)attributes=static_cast<std::uint16_t>((attributes&~15U)|colors[value-90]|8U);
            else if(value>=100 && value<=107)attributes=static_cast<std::uint16_t>((attributes&~0xf0U)|(colors[value-100]<<4)|0x80U);
            else if(value==39)attributes=static_cast<std::uint16_t>((attributes&~15U)|7U);
            else if(value==49)attributes&=~0xf0U;
        }
    }
}
void ConsoleBuffer::write(std::string_view value){
    if(value.empty())return;
    output_utf8.append(value);
    for(std::size_t offset=0;offset<output_utf8.size();){
        const auto byte=static_cast<unsigned char>(output_utf8[offset]);
        if(!escape.empty()){
            escape+=static_cast<char>(byte);++offset;
            if(escape.size()>4096)escape.clear();
            else if(escape.size()==2 && byte!='[' && byte!=']')escape.clear();
            else if(escape.size()>2 && escape[1]=='[' && byte>=0x40 && byte<=0x7e){control_sequence();escape.clear();}
            else if(escape.size()>2 && escape[1]==']' && (byte==7 || (byte=='\\' && escape[escape.size()-2]==27))){
                auto text=std::string_view(escape).substr(2);const auto separator=text.find(';');
                if(separator!=text.npos && (text.substr(0,separator)=="0" || text.substr(0,separator)=="2")){
                    text.remove_prefix(separator+1);text.remove_suffix(byte==7?1:2);title=utf8_to_utf16_lossy(text);
                }escape.clear();
            }
        }else if(byte==27 && (output_mode&4)){escape.assign(1,27);++offset;}
        else {
            const unsigned extent=byte<128?1:byte>=0xc2 && byte<=0xdf?2:byte>=0xe0 && byte<=0xef?3:byte>=0xf0 && byte<=0xf4?4:1;
            if(offset+extent>output_utf8.size()){output_utf8.erase(0,offset);return;}
            for(auto c:utf8_to_utf16_lossy(std::string_view(output_utf8).substr(offset,extent)))character(c);
            offset+=extent;
        }
    }output_utf8.clear();reveal_cursor();
}
void ConsoleBuffer::ingest(std::string_view value,bool final){
    terminal_input.append(value);
    std::size_t offset=0;
    while(offset<terminal_input.size()){
        auto text=std::string_view(terminal_input).substr(offset);
        if(text.starts_with("\x1b[") || text.starts_with("\x1bO")){
            std::size_t end=2;while(end<text.size() && !(text[end]>='@' && text[end]<='~'))++end;
            if(end==text.size() && !final)break;
            if(end<text.size()){
                std::uint16_t key=0,scan=0;switch(text[end]){case 'A':key=38;scan=72;break;case 'B':key=40;scan=80;break;case 'C':key=39;scan=77;break;case 'D':key=37;scan=75;break;case 'H':key=36;scan=71;break;case 'F':key=35;scan=79;break;default:break;}
                if(text[end]=='~'){unsigned number=0;const auto number_end=text.find(';');const auto part=text.substr(2,(number_end==text.npos?end:number_end)-2);(void)std::from_chars(part.data(),part.data()+part.size(),number);
                    if(number==1 || number==7){key=36;scan=71;}else if(number==2){key=45;scan=82;}else if(number==3){key=46;scan=83;}else if(number==4 || number==8){key=35;scan=79;}else if(number==5){key=33;scan=73;}else if(number==6){key=34;scan=81;}}
                if(key){push(console_key(0,true,key,scan,0x100));offset+=end+1;continue;}
            }
        }
        const auto lead=static_cast<unsigned char>(text[0]);const unsigned extent=lead<128?1:lead>=0xc2 && lead<=0xdf?2:lead>=0xe0 && lead<=0xef?3:lead>=0xf0 && lead<=0xf4?4:1;
        if(text.size()<extent && !final)break;
        const auto length=std::min<std::size_t>(extent,text.size());
        for(auto c:utf8_to_utf16_lossy(text.substr(0,length)))push(console_key(c==u'\n'?u'\r':c));
        offset+=length;
    }
    terminal_input.erase(0,offset);
}
NativeConsole::~NativeConsole(){
#if !defined(_WIN32)
    if(descriptor>=0){if(changed)(void)::tcsetattr(descriptor,TCSANOW,&original);::close(descriptor);}
#endif
}
#if !defined(__APPLE__)
std::unique_ptr<ConsoleWindow> create_console_window(){return {};}
#endif
}
