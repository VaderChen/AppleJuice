// 僅折回轉譯器產生的直線指派區塊，不重排浮點運算，也不改變算術精度。
// 每份區塊除相同的十六進位陣列下標外必須逐字一致；下標逐份增加一。
std::string metal_compact_compute_loops(const std::string& source,unsigned block=1){
    if(!block || block>16 || (block&(block-1)))return source;
    constexpr std::string_view induction="x64arm_compacted_index_";
    if(source.find(induction)!=std::string::npos)return source;
    struct Line {std::string text,normalized;std::vector<unsigned> indices;};
    const std::regex index(R"(\[0x([0-9a-f]+)\])");
    const std::regex assignment(R"(^\s*\w+(?:\.\w+)?\s*=)");
    const std::regex declaration(R"(^\s*auto\s+\w+\s*=)");
    std::vector<Line> lines;
    std::istringstream input(source);std::string text;
    while(std::getline(input,text)){
        Line line{text+"\n",{}, {}};line.normalized=std::regex_replace(line.text,index,"[INDEX]");
        for(std::sregex_iterator it(line.text.begin(),line.text.end(),index),end;it!=end;++it){
            const auto digits=(*it)[1].str();if(digits.size()>7)return source;
            line.indices.push_back(static_cast<unsigned>(std::stoul(digits,nullptr,16)));
        }
        lines.push_back(std::move(line));
    }
    std::string output;unsigned serial=0;
    for(std::size_t at=0;at<lines.size();){
        std::size_t period=0,copies=0;unsigned initial=0;
        if(!lines[at].indices.empty())for(std::size_t length=1;length<=64 && at+length*8<=lines.size();++length){
            int depth=0;bool safe=true;const auto first=lines[at].indices.front();
            for(std::size_t n=0;n<length && safe;++n){
                const auto& line=lines[at+n];const auto begin=line.text.find_first_not_of(" \t\r\n");
                const auto token=begin==std::string::npos?std::string{}:line.text.substr(begin,line.text.find_last_not_of(" \t\r\n")-begin+1);
                if(token=="{")++depth;
                else if(token=="}"){if(--depth<0)safe=false;}
                else if(token.empty() || token.back()!=';' || std::count(token.begin(),token.end(),';')!=1 || token.find_first_of("{}#\"'")!=std::string::npos || token.find("//")!=std::string::npos || token.find("/*")!=std::string::npos ||
                        (!std::regex_search(line.text,assignment) && !(depth>0 && std::regex_search(line.text,declaration))))safe=false;
                for(auto value:line.indices)if(value!=first)safe=false;
            }
            if(!safe || depth)continue;
            std::size_t count=1;
            while(at+(count+1)*length<=lines.size() && std::uint64_t{first}+count<0x7fffffff){
                bool same=true;
                for(std::size_t n=0;n<length && same;++n){
                    const auto& original=lines[at+n];const auto& next=lines[at+count*length+n];
                    if(original.normalized!=next.normalized || original.indices.size()!=next.indices.size()){same=false;break;}
                    for(auto value:next.indices)if(value!=first+count)same=false;
                }
                if(!same)break;++count;
            }
            if(count>=8){period=length;copies=count;initial=first;break;}
        }
        if(!period){output+=lines[at++].text;continue;}
        auto chunk=block;while(chunk>copies/2)chunk/=2;copies-=copies%chunk;
        const auto name=std::string(induction)+std::to_string(serial++);
        output+="#pragma clang loop unroll(disable)\nfor (int "+name+" = "+std::to_string(initial)+"; "+name+" < "+std::to_string(initial+copies)+"; "+name+" += "+std::to_string(chunk)+") {\n";
        for(unsigned part=0;part<chunk;++part)for(std::size_t n=0;n<period;++n)output+=std::regex_replace(lines[at+n].text,index,"["+name+(part?" + "+std::to_string(part):"")+"]");
        output+="}\n";at+=period*copies;
    }
    return serial?output:source;
}
