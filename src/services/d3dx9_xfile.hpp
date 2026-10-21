#pragma once
#include <source_location>
// X-file 文字／二進位 tokenizer；數值列表展開後共用同一組 mesh parser。
namespace x64arm::d3d9_xfile {
struct Token {std::string text;double number{};bool numeric{};std::array<std::uint8_t,16> guid{};bool uuid{};};
struct EffectDefault {std::string name;unsigned type{1};std::vector<std::uint8_t> bytes;};
struct Material {std::array<float,17> values{};std::string texture,effect;std::vector<EffectDefault> defaults;};
struct SkinWeights {std::string name;std::vector<unsigned> vertices;std::vector<float> weights;std::array<float,16> offset{};};
struct Mesh {std::string name;std::vector<unsigned> original_vertices;std::vector<SkinWeights> bones;std::vector<std::array<float,8>> vertices;std::vector<unsigned> indices,attributes,colors;std::vector<Material> materials;bool normals{},uv{},has_colors{};};
struct Frame {std::string name;std::array<float,16> transform{};std::vector<Mesh> meshes;std::vector<Frame> children;Frame(){transform[0]=transform[5]=transform[10]=transform[15]=1;}};
struct AnimationKey {double time{};std::vector<float> values;};
struct Animation {std::string name;std::map<unsigned,std::vector<AnimationKey>> keys;};
struct AnimationSet {std::string name;std::vector<Animation> animations;};
struct Hierarchy {std::vector<Frame> frames;std::vector<AnimationSet> sets;double ticks_per_second{4800};};
inline std::vector<std::uint8_t> decompress(std::span<const std::uint8_t> source){
    const auto check=[](bool ok){if(!ok)throw std::invalid_argument("無效的 MSZIP X-file");};
    check(source.size()>=16);if(std::memcmp(source.data()+8,"bzip",4) && std::memcmp(source.data()+8,"tzip",4))return {source.begin(),source.end()};
    check(source.size()>=20);std::uint32_t total{};std::memcpy(&total,source.data()+16,4);check(total>=16 && total<=512U*1024U*1024U);
    std::vector<std::uint8_t> result(source.begin(),source.begin()+16);std::memcpy(result.data()+8,source[8]=='b'?"bin ":"txt ",4);std::size_t at=20;
    while(at<source.size()){
        check(source.size()-at>=6);const unsigned output=unsigned(source[at])|unsigned(source[at+1])<<8,input=unsigned(source[at+2])|unsigned(source[at+3])<<8;at+=4;check(output && output<=32768 && input>=2 && input<=source.size()-at && source[at]=='C' && source[at+1]=='K');
        z_stream stream{};check(inflateInit2(&stream,-MAX_WBITS)==Z_OK);const auto begin=result.size();result.resize(begin+output);if(begin>16){const auto size=std::min<std::size_t>(32768,begin-16);if(inflateSetDictionary(&stream,result.data()+begin-size,static_cast<uInt>(size))!=Z_OK){inflateEnd(&stream);check(false);}}
        stream.next_in=const_cast<Bytef*>(source.data()+at+2);stream.avail_in=input-2;stream.next_out=result.data()+begin;stream.avail_out=output;const auto status=inflate(&stream,Z_FINISH);const auto written=stream.total_out;inflateEnd(&stream);check(status==Z_STREAM_END && written==output);at+=input;check(result.size()<=total);
    }check(result.size()==total);return result;
}
class Parser {
    std::vector<Token> tokens;std::size_t cursor{};
    static void check(bool ok,const std::source_location where=std::source_location::current()){if(!ok)throw std::invalid_argument("無效的 DirectX X-file，解析檢查行 "+std::to_string(where.line()));}
    bool is(std::string_view s)const{return cursor<tokens.size() && tokens[cursor].text==s;}
    void punctuation(){while(is(";") || is(","))++cursor;}
    double number(){punctuation();check(cursor<tokens.size() && tokens[cursor].numeric);return tokens[cursor++].number;}
    unsigned integer(){const auto n=number();check(n>=0 && n<=16777216 && std::floor(n)==n);return static_cast<unsigned>(n);}
    void brace(){if(!is("{")){check(cursor<tokens.size());++cursor;}check(is("{"));++cursor;}
    void skip(){brace();unsigned depth=1;while(cursor<tokens.size() && depth){if(is("{"))++depth;if(is("}"))--depth;++cursor;}check(!depth);}
    void effect_instance(Material& m){
        brace();punctuation();check(cursor<tokens.size());m.effect=tokens[cursor++].text;
        while(cursor<tokens.size()){punctuation();if(is("}")){++cursor;return;}const auto name=tokens[cursor++].text;
            if(name!="EffectParamString" && name!="EffectParamFloats" && name!="EffectParamDWord"){skip();continue;}
            brace();punctuation();check(cursor<tokens.size());EffectDefault value;value.name=tokens[cursor++].text;punctuation();
            if(name=="EffectParamString"){check(cursor<tokens.size());const auto text=tokens[cursor++].text;value.bytes.assign(text.begin(),text.end());value.bytes.push_back(0);}
            else {value.type=name=="EffectParamFloats"?2:3;const unsigned count=value.type==2?integer():1;check(count<=1048576);value.bytes.resize(count*4);
                for(unsigned i=0;i<count;++i){const auto n=number();check(std::isfinite(n));std::uint32_t raw{};if(value.type==2)raw=std::bit_cast<std::uint32_t>(static_cast<float>(n));else{check(n>=0 && n<=4294967295. && std::floor(n)==n);raw=static_cast<std::uint32_t>(n);}std::memcpy(value.bytes.data()+i*4,&raw,4);}}
            punctuation();check(is("}"));++cursor;m.defaults.push_back(std::move(value));
        }check(false);
    }
    Material material(){Material m;brace();for(unsigned i=0;i<4;++i)m.values[i]=static_cast<float>(number());m.values[16]=static_cast<float>(number());for(unsigned i=8;i<11;++i)m.values[i]=static_cast<float>(number());for(unsigned i=12;i<15;++i)m.values[i]=static_cast<float>(number());std::copy_n(m.values.begin(),4,m.values.begin()+4);
        while(cursor<tokens.size()){punctuation();if(is("}")){++cursor;break;}const auto name=tokens[cursor++].text;if(name=="TextureFilename"){brace();check(cursor<tokens.size());m.texture=tokens[cursor++].text;punctuation();check(is("}"));++cursor;}else if(name=="EffectInstance")effect_instance(m);else skip();}return m;}
    void mesh(Mesh& result,const std::array<float,16>& matrix){
        const auto vertex_base=result.vertices.size();if(!is("{"))result.name=tokens.at(cursor).text;std::vector<SkinWeights> skin_weights;
        brace();const auto n=integer();std::vector<std::array<float,3>> pos(n),norm;std::vector<std::array<float,2>> uv;std::vector<unsigned> colors;for(auto& p:pos)for(auto& c:p)c=static_cast<float>(number());
        const auto faces=integer();std::vector<std::vector<unsigned>> polygons(faces),normal_faces;
        for(auto& face:polygons){const auto count=integer();check(count>=3 && count<=65536);face.resize(count);for(auto& i:face){i=integer();check(i<n);}}
        std::vector<unsigned> attributes(faces);std::vector<Material> materials;
        while(cursor<tokens.size()){
            punctuation();if(is("}")){++cursor;break;}check(cursor<tokens.size());const auto name=tokens[cursor++].text;
            if(name=="SkinWeights"){brace();punctuation();check(cursor<tokens.size());SkinWeights bone;bone.name=tokens[cursor++].text;const auto count=integer();check(count<=n);bone.vertices.resize(count);bone.weights.resize(count);for(auto& v:bone.vertices){v=integer();check(v<n);}for(auto& weight:bone.weights){weight=static_cast<float>(number());check(std::isfinite(weight) && weight>=0);}for(auto& value:bone.offset){value=static_cast<float>(number());check(std::isfinite(value));}punctuation();check(is("}"));++cursor;skin_weights.push_back(std::move(bone));}
            else if(name=="MeshNormals"){brace();norm.resize(integer());for(auto& p:norm)for(auto& c:p)c=static_cast<float>(number());normal_faces.resize(integer());check(normal_faces.size()==faces);for(unsigned f=0;f<faces;++f){auto& v=normal_faces[f];v.resize(integer());check(v.size()==polygons[f].size());for(auto& i:v){i=integer();check(i<norm.size());}}punctuation();check(is("}"));++cursor;}
            else if(name=="MeshTextureCoords"){brace();uv.resize(integer());check(uv.size()==n);for(auto& p:uv)for(auto& c:p)c=static_cast<float>(number());punctuation();check(is("}"));++cursor;}
            else if(name=="MeshVertexColors"){brace();const auto count=integer();check(count<=n);colors.assign(n,0xffffffffU);for(unsigned i=0;i<count;++i){const auto index=integer();check(index<n);unsigned color=0;constexpr unsigned shifts[]{16,8,0,24};for(unsigned c=0;c<4;++c){const auto value=number();check(std::isfinite(value));color|=static_cast<unsigned>(std::clamp(value,0.,1.)*255.+0.5)<<shifts[c];}colors[index]=color;}punctuation();check(is("}"));++cursor;}
            else if(name=="MeshMaterialList"){brace();const auto count=integer(),attributes_count=integer();check(attributes_count==faces || attributes_count==1);for(unsigned f=0;f<attributes_count;++f){attributes[f]=integer();check(attributes[f]<count);}if(attributes_count==1)std::fill(attributes.begin(),attributes.end(),attributes[0]);
                while(cursor<tokens.size()){punctuation();if(is("}")){++cursor;break;}if(is("Material")){++cursor;materials.push_back(material());}else if(is("{")){++cursor;check(cursor<tokens.size());const auto reference=tokens[cursor++].text;check(named_materials.contains(reference));materials.push_back(named_materials.at(reference));check(is("}"));++cursor;}else{++cursor;skip();}}check(materials.size()==count);
            }else skip();
        }
        if(materials.empty()){Material m;m.values[0]=m.values[1]=m.values[2]=m.values[3]=1;materials.push_back(m);}
        // 沒有 EffectInstance 的材質仍產生與 D3DMATERIAL9 對應的預設參數。
        for(auto& m:materials)if(m.effect.empty() && m.defaults.empty()){
            for(auto [name,offset,count]:{std::tuple{"Diffuse",0U,4U},std::tuple{"Ambient",4U,4U},std::tuple{"Specular",8U,4U},std::tuple{"Emissive",12U,4U},std::tuple{"Power",16U,1U}}){EffectDefault d;d.name=name;d.type=2;d.bytes.resize(count*4);std::memcpy(d.bytes.data(),m.values.data()+offset,count*4);m.defaults.push_back(std::move(d));}
            if(!m.texture.empty()){EffectDefault d;d.name="Texture0@Name";d.type=1;d.bytes.assign(m.texture.begin(),m.texture.end());d.bytes.push_back(0);m.defaults.push_back(std::move(d));}
        }
        const auto material_base=static_cast<unsigned>(result.materials.size());result.materials.insert(result.materials.end(),materials.begin(),materials.end());
        std::map<std::pair<unsigned,unsigned>,unsigned> remap;
        for(unsigned f=0;f<faces;++f){std::vector<unsigned> face;for(unsigned corner=0;corner<polygons[f].size();++corner){const auto v=polygons[f][corner],ni=normal_faces.empty()?UINT32_MAX:normal_faces[f][corner];const auto key=std::pair{v,ni};auto it=remap.find(key);if(it==remap.end()){
                    std::array<float,8> data{};for(unsigned c=0;c<3;++c){data[c]=matrix[12+c];for(unsigned r=0;r<3;++r)data[c]+=pos[v][r]*matrix[r*4+c];}
                    if(ni!=UINT32_MAX){
                        // 法線使用反矩陣轉置，維持含非等比縮放的 Frame 契約。
                        const double determinant=double(matrix[0])*(double(matrix[5])*matrix[10]-double(matrix[6])*matrix[9])-double(matrix[1])*(double(matrix[4])*matrix[10]-double(matrix[6])*matrix[8])+double(matrix[2])*(double(matrix[4])*matrix[9]-double(matrix[5])*matrix[8]);check(determinant!=0);
                        for(unsigned c=0;c<3;++c)for(unsigned r=0;r<3;++r){const auto r1=(r+1)%3,r2=(r+2)%3,c1=(c+1)%3,c2=(c+2)%3;data[3+c]+=static_cast<float>(norm[ni][r]*(double(matrix[r1*4+c1])*matrix[r2*4+c2]-double(matrix[r1*4+c2])*matrix[r2*4+c1])/determinant);}
                        const auto length=std::sqrt(data[3]*data[3]+data[4]*data[4]+data[5]*data[5]);if(length>0)for(unsigned c=3;c<6;++c)data[c]/=length;
                    }
                    if(!uv.empty()){data[6]=uv[v][0];data[7]=uv[v][1];}const auto index=static_cast<unsigned>(result.vertices.size());result.vertices.push_back(data);result.original_vertices.push_back(v);result.colors.push_back(colors.empty()?0xffffffffU:colors[v]);it=remap.emplace(key,index).first;
                }face.push_back(it->second);}
            for(unsigned j=1;j+1<face.size();++j){result.indices.insert(result.indices.end(),{face[0],face[j],face[j+1]});result.attributes.push_back(material_base+attributes[f]);}
        }
        for(auto& original:skin_weights){SkinWeights bone;bone.name=std::move(original.name);bone.offset=original.offset;std::map<unsigned,float> weights;for(std::size_t i=0;i<original.vertices.size();++i){check(weights.emplace(original.vertices[i],original.weights[i]).second);}for(std::size_t i=vertex_base;i<result.vertices.size();++i)if(auto found=weights.find(result.original_vertices[i]);found!=weights.end()){bone.vertices.push_back(static_cast<unsigned>(i));bone.weights.push_back(found->second);}result.bones.push_back(std::move(bone));}
        result.normals|=!norm.empty();result.uv|=!uv.empty();result.has_colors|=!colors.empty();
    }
    std::map<std::string,Material> named_materials;
    void objects(Mesh& out,std::array<float,16> parent,bool nested){
        auto matrix=parent;
        // FrameTransformMatrix 的位置不限制於 Mesh 之前；先讀取此層的變換。
        unsigned depth=0;
        for(std::size_t scan=cursor;scan<tokens.size();++scan){
            const auto& token=tokens[scan];if(token.text=="}" && !depth)break;
            // 模板名稱不是資料物件；模板宣告中的同名成員不能當成矩陣值。
            if(!depth && token.text=="template"){
                while(scan<tokens.size() && tokens[scan].text!="{")++scan;
                check(scan<tokens.size());unsigned declaration_depth=1;
                while(++scan<tokens.size() && declaration_depth){if(tokens[scan].text=="{")++declaration_depth;else if(tokens[scan].text=="}")--declaration_depth;}
                check(!declaration_depth);--scan;continue;
            }
            if(!depth && token.text=="FrameTransformMatrix"){
                std::size_t at=scan+1;while(at<tokens.size() && tokens[at].text!="{")++at;check(at<tokens.size());++at;std::array<float,16> local{},combined{};
                for(auto& value:local){while(at<tokens.size() && (tokens[at].text==";" || tokens[at].text==","))++at;check(at<tokens.size() && tokens[at].numeric);value=static_cast<float>(tokens[at++].number);}
                for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)for(unsigned k=0;k<4;++k)combined[r*4+c]+=local[r*4+k]*parent[k*4+c];matrix=combined;
            }
            if(token.text=="{")++depth;else if(token.text=="}")--depth;
        }
        while(cursor<tokens.size()){
            punctuation();if(is("}")){check(nested);++cursor;return;}check(cursor<tokens.size());const auto name=tokens[cursor++].text;
            if(name=="template"){check(cursor<tokens.size());++cursor;skip();}
            else if(name=="Frame"){brace();objects(out,matrix,true);}
            else if(name=="FrameTransformMatrix"){brace();std::array<float,16> local{},combined{};for(auto& x:local)x=static_cast<float>(number());for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)for(unsigned k=0;k<4;++k)combined[r*4+c]+=local[r*4+k]*parent[k*4+c];matrix=combined;punctuation();check(is("}"));++cursor;}
            else if(name=="Mesh")mesh(out,matrix);
            else if(name=="Material"){const auto id=is("{")?std::string{}:tokens[cursor].text;auto m=material();if(!id.empty())named_materials[id]=std::move(m);}
            else skip();
        }check(!nested);
    }
#include "d3dx9_hierarchy_parser.inc"
public:
    explicit Parser(std::span<const std::uint8_t> data){
        check(data.size()>=16 && std::memcmp(data.data(),"xof ",4)==0);const std::string format(reinterpret_cast<const char*>(data.data()+8),4);const bool wide=std::memcmp(data.data()+12,"0064",4)==0;std::size_t at=16;
        const auto take=[&](unsigned bytes){check(at<=data.size() && bytes<=data.size()-at);std::uint64_t n=0;for(unsigned i=0;i<bytes;++i)n|=std::uint64_t(data[at++])<<(i*8);return n;};
        if(format=="bin "){
            while(at<data.size()){const auto token=take(2);if(token==1 || token==2){const auto size=take(4);check(size<=data.size()-at);tokens.push_back({std::string(reinterpret_cast<const char*>(data.data()+at),size),0,false});at+=size;if(token==2)check(take(2)==20);}
                else if(token==3)tokens.push_back({{},double(take(4)),true});
                else if(token==6 || token==7){const auto count=take(4);check(count<=16777216);for(std::uint64_t i=0;i<count;++i){double v;if(token==6)v=double(take(4));else if(wide)v=std::bit_cast<double>(take(8));else v=std::bit_cast<float>(static_cast<std::uint32_t>(take(4)));tokens.push_back({{},v,true});}}
                else if(token==5){Token value{"guid"};value.uuid=true;for(auto& c:value.guid)c=static_cast<std::uint8_t>(take(1));tokens.push_back(value);}
                else if(token==10)tokens.push_back({"{"});else if(token==11)tokens.push_back({"}"});else if(token==19)tokens.push_back({","});else if(token==20)tokens.push_back({";"});else if(token==31)tokens.push_back({"template"});else if(token>=12 && token<=52){const std::map<unsigned,std::string> names{{12,"("},{13,")"},{14,"["},{15,"]"},{16,"<"},{17,">"},{18,"."},{40,"WORD"},{41,"DWORD"},{42,"FLOAT"},{43,"DOUBLE"},{44,"CHAR"},{45,"UCHAR"},{46,"SWORD"},{47,"SDWORD"},{48,"VOID"},{49,"LPSTR"},{50,"UNICODE"},{51,"CSTRING"},{52,"array"}};tokens.push_back({names.contains(static_cast<unsigned>(token))?names.at(static_cast<unsigned>(token)):"type"});}else check(false);
            }
        }else if(format=="txt "){
            const std::string text(reinterpret_cast<const char*>(data.data()+at),data.size()-at);at=0;
            while(at<text.size()){
                const char c=text[at];if(std::isspace(static_cast<unsigned char>(c))){++at;continue;}if(c=='#' || (c=='/' && at+1<text.size() && text[at+1]=='/')){while(at<text.size() && text[at]!='\n')++at;continue;}
                if(c=='<'){const auto end=text.find('>',at+1);check(end!=std::string::npos);std::string hex;for(std::size_t i=at+1;i<end;++i)if(text[i]!='-'){check(std::isxdigit(static_cast<unsigned char>(text[i])));hex+=text[i];}check(hex.size()==32);Token value{"guid"};value.uuid=true;constexpr unsigned order[]{3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};for(unsigned i=0;i<16;++i)value.guid[order[i]]=static_cast<std::uint8_t>(std::stoul(hex.substr(i*2,2),nullptr,16));tokens.push_back(value);at=end+1;}
                else if(c=='"'){++at;std::string value;while(at<text.size() && text[at]!='"'){if(text[at]=='\\' && at+1<text.size() && (text[at+1]=='\\' || text[at+1]=='"'))++at;value+=text[at++];}check(at<text.size());++at;tokens.push_back({std::move(value)});}
                else if(std::isdigit(static_cast<unsigned char>(c)) || c=='-' || c=='+' || (c=='.' && at+1<text.size() && std::isdigit(static_cast<unsigned char>(text[at+1])))){char* end=nullptr;const auto v=std::strtod(text.c_str()+at,&end);check(end>text.c_str()+at);at=static_cast<std::size_t>(end-text.c_str());tokens.push_back({{},v,true});}
                else if(std::strchr("{};,<>[]",c)){tokens.push_back({std::string(1,c)});++at;}
                else{const auto first=at;while(at<text.size() && !std::isspace(static_cast<unsigned char>(text[at])) && !std::strchr("{};,<>[]",text[at]))++at;check(at>first);tokens.push_back({text.substr(first,at-first)});}
            }
        }else check(false);
    }
    Hierarchy hierarchy(){cursor=0;Hierarchy result;hierarchy_objects(result,nullptr);return result;}
    const std::vector<Token>& token_stream()const{return tokens;}
    Mesh parse(){Mesh out;std::array<float,16> identity{};identity[0]=identity[5]=identity[10]=identity[15]=1;objects(out,identity,false);check(!out.vertices.empty() && !out.indices.empty());return out;}
};
}
