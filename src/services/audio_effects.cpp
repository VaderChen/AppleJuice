#include "x64arm/audio_effects.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace x64arm::audio {
namespace {
constexpr double pi=3.14159265358979323846;
float db(float value){return std::pow(10.0F,value/20);}
std::uint32_t word(float value){return std::bit_cast<std::uint32_t>(value);}
// SDK I3DL2 environment presets；各欄位的數值及順序來自 dsound.h。
constexpr float presets[][12]={
    {-1000,-100,0,1.49F,.83F,-2602,.007F,200,.011F,100,100,5000},
    {-1000,-100,0,1.49F,.83F,-2602,.007F,200,.011F,100,100,5000},
    {-1000,-6000,0,.17F,.10F,-1204,.001F,207,.002F,100,100,5000},
    {-1000,-454,0,.40F,.83F,-1646,.002F,53,.003F,100,100,5000},
    {-1000,-1200,0,1.49F,.54F,-370,.007F,1030,.011F,100,60,5000},
    {-1000,-6000,0,.50F,.10F,-1376,.003F,-1104,.004F,100,100,5000},
    {-1000,-300,0,2.31F,.64F,-711,.012F,83,.017F,100,100,5000},
    {-1000,-476,0,4.32F,.59F,-789,.020F,-289,.030F,100,100,5000},
    {-1000,-500,0,3.92F,.70F,-1230,.020F,-2,.029F,100,100,5000},
    {-1000,0,0,2.91F,1.30F,-602,.015F,-302,.022F,100,100,5000},
    {-1000,-698,0,7.24F,.33F,-1166,.020F,16,.030F,100,100,5000},
    {-1000,-1000,0,10.05F,.23F,-602,.020F,198,.030F,100,100,5000},
    {-1000,-4000,0,.30F,.10F,-1831,.002F,-1630,.030F,100,100,5000},
    {-1000,-300,0,1.49F,.59F,-1219,.007F,441,.011F,100,100,5000},
    {-1000,-237,0,2.70F,.79F,-1214,.013F,395,.020F,100,100,5000},
    {-1000,-270,0,1.49F,.86F,-1204,.007F,-4,.011F,100,100,5000},
    {-1000,-3300,0,1.49F,.54F,-2560,.162F,-613,.088F,79,100,5000},
    {-1000,-800,0,1.49F,.67F,-2273,.007F,2217,.011F,50,100,5000},
    {-1000,-2500,0,1.49F,.21F,-2780,.300F,-2014,.100F,27,100,5000},
    {-1000,-1000,0,1.49F,.83F,-10000,.061F,500,.025F,100,100,5000},
    {-1000,-2000,0,1.49F,.50F,-2466,.179F,-2514,.100F,21,100,5000},
    {-1000,0,0,1.65F,1.50F,-1363,.008F,-1153,.012F,100,100,5000},
    {-1000,-1000,0,2.81F,.14F,429,.014F,648,.011F,80,60,5000},
    {-1000,-4000,0,1.49F,.10F,-449,.007F,1700,.011F,100,100,5000},
    {-1000,-600,0,1.10F,.83F,-400,.005F,500,.010F,100,100,5000},
    {-1000,-600,0,1.30F,.83F,-1000,.010F,-200,.020F,100,100,5000},
    {-1000,-600,0,1.50F,.83F,-1600,.020F,-1000,.040F,100,100,5000},
    {-1000,-600,0,1.80F,.70F,-1300,.015F,-800,.030F,100,100,5000},
    {-1000,-600,0,1.80F,.70F,-2000,.030F,-1400,.060F,100,100,5000},
    {-1000,-200,0,1.30F,.90F,0,.002F,0,.010F,100,75,5000}
};
bool integer(Effect::Type type,unsigned index){return type==Effect::Type::Gargle || ((type==Effect::Type::Chorus || type==Effect::Type::Flanger) && (index==4 || index==6)) || (type==Effect::Type::Echo && index==4) || (type==Effect::Type::I3DL2Reverb && (index==0 || index==1 || index==5 || index==7));}
std::array<std::uint32_t,12> preset_words(unsigned index){std::array<std::uint32_t,12> words{};for(unsigned i=0;i<12;++i)words[i]=integer(Effect::Type::I3DL2Reverb,i)?static_cast<std::uint32_t>(static_cast<std::int32_t>(presets[index][i])):word(presets[index][i]);return words;}
}
struct Effect::State {
    Type type;unsigned rate,quality{2},preset{UINT32_MAX},count{};
    std::array<std::uint32_t,12> words{};
    std::array<float,12> p{};
    std::array<std::vector<float>,2> delay;
    std::size_t cursor{};double phase{};
    std::array<float,2> low{},envelope{},tone{};
    struct Biquad {double b0{1},b1{},b2{},a1{},a2{},z1{},z2{};float run(float x){const double y=b0*x+z1;z1=b1*x-a1*y+z2;z2=b2*x-a2*y;return static_cast<float>(y);}};
    std::array<Biquad,2> eq;
    struct Comb {std::vector<float> data;std::size_t cursor{},length{};float damp{},feedback{},feedback_hf{};};
    std::array<std::array<Comb,8>,2> comb;
    unsigned combs{6};float damping{},input_filter{},early_gain{},wet_gain{},input_gain{1},high_gain{1},diffusion{1},attack{},release{},output_gain{1};
    double left_delay{},right_delay{},early_delay{},late_delay{};
    double phase_step{};
    State(Type t,unsigned r):type(t),rate(r){
        if(r<100 || r>200000 || static_cast<unsigned>(t)>8)throw std::invalid_argument("音訊效果種類或取樣率無效");
        // Gargle 不經過延遲流程；另外兩種不讀延遲的效果只保留共用單槽。
        const auto frames=t==Type::Gargle?std::size_t{0}:
            t==Type::Distortion || t==Type::ParamEq?std::size_t{1}:static_cast<std::size_t>(r)*2+8;
        for(auto& channel:delay)channel.resize(frames);
        if(t==Type::I3DL2Reverb || t==Type::WavesReverb)for(auto& channel:comb)for(auto& line:channel)line.data.resize(static_cast<std::size_t>(r)/4+8);
    }
    void configure(){
        for(unsigned i=0;i<count;++i)p[i]=integer(type,i)?static_cast<float>(static_cast<std::int32_t>(words[i])):std::bit_cast<float>(words[i]);
        if(type==Type::Gargle)phase_step=static_cast<double>(p[0])/rate;
        else if(type==Type::Chorus || type==Type::Flanger)phase_step=static_cast<double>(p[3])/rate;
        const auto filter=[&](float hz){return static_cast<float>(1-std::exp(-2*pi*std::min(static_cast<double>(hz),rate*.45)/rate));};
        if(type==Type::ParamEq || type==Type::Distortion){
            const auto center=type==Type::ParamEq?p[0]:p[2];const auto w=2*pi*std::min(static_cast<double>(center),rate*.45)/rate;const auto sine=std::sin(w),cosine=std::cos(w);
            const auto alpha=type==Type::ParamEq?sine*std::sinh(std::log(2.0)*.5*(p[1]/12)*w/sine):sine/(2*std::max(.05,static_cast<double>(center)/p[3]));
            const auto a=type==Type::ParamEq?std::pow(10.0,p[2]/40):1.0;const auto divisor=1+alpha/a;
            for(auto& f:eq){f.b0=type==Type::ParamEq?(1+alpha*a)/divisor:alpha/divisor;f.b1=type==Type::ParamEq?-2*cosine/divisor:0;f.b2=type==Type::ParamEq?(1-alpha*a)/divisor:-alpha/divisor;f.a1=-2*cosine/divisor;f.a2=(1-alpha/a)/divisor;}
            if(type==Type::Distortion){input_filter=filter(p[4]);output_gain=db(p[0]);}
        }else if(type==Type::Echo){left_delay=p[2]*static_cast<double>(rate)/1000.0;right_delay=p[3]*static_cast<double>(rate)/1000.0;}
        else if(type==Type::Compressor){attack=static_cast<float>(std::exp(-1.0/(p[1]*static_cast<double>(rate)/1000.0)));release=static_cast<float>(std::exp(-1.0/(p[2]*static_cast<double>(rate)/1000.0)));left_delay=right_delay=p[5]*static_cast<double>(rate)/1000.0;output_gain=db(p[0]);}
        else if(type==Type::I3DL2Reverb || type==Type::WavesReverb){
            const bool i3=type==Type::I3DL2Reverb;combs=i3?2+quality*2:6;const auto decay=i3?p[3]:p[2]/1000;const auto density=i3?p[10]/100:1;
            high_gain=i3?db(p[1]/100):1;diffusion=i3?p[9]/100:1;
            damping=filter(i3?p[11]:5000);input_filter=filter(i3?p[11]:20000);
            early_delay=i3?p[6]*static_cast<double>(rate):0;late_delay=i3?p[8]*static_cast<double>(rate):0;early_gain=i3?db(p[5]/100):0;wet_gain=i3?db(p[7]/100):db(p[1]);input_gain=db(i3?p[0]/100:p[0]);
            constexpr std::array lengths{.0297,.0371,.0411,.0437,.0531,.0617,.0719,.0793};
            for(unsigned c=0;c<2;++c)for(unsigned i=0;i<8;++i){auto& line=comb[c][i];line.length=std::clamp<std::size_t>(static_cast<std::size_t>(rate*(lengths[i]*(.3+.7*density)+c*.0013)),1,line.data.size());line.cursor%=line.length;const auto seconds=static_cast<double>(line.length)/rate;line.feedback=static_cast<float>(std::pow(.001,seconds/decay));line.feedback_hf=static_cast<float>(std::pow(.001,seconds/(decay*(i3?p[4]:p[3]))));}
        }
    }
    float delayed(unsigned channel,double frames)const{
        const auto& samples=delay[channel];const double position=static_cast<double>(cursor)+static_cast<double>(samples.size())-std::clamp(frames,1.0,static_cast<double>(samples.size()-2));
        const auto index=static_cast<std::size_t>(position)%samples.size();const auto fraction=static_cast<float>(position-std::floor(position));return samples[index]*(1-fraction)+samples[(index+1)%samples.size()]*fraction;
    }
    void advance_phase(){
        const auto next=phase+phase_step;
        // 常見取樣率只需一次折返；低取樣率仍保留多圈 fmod 的契約。
        phase=next<1?next:next<2?next-1:std::fmod(next,1.0);
    }
};
Effect::Effect(Type type,unsigned rate):state_(std::make_unique<State>(type,rate)){
    auto& s=*state_;std::vector<float> defaults;
    switch(type){
    case Type::Gargle:s.count=2;s.words[0]=20;s.words[1]=0;break;
    case Type::Chorus:defaults={50,10,25,1.1F,1,16,3};break;
    case Type::Flanger:defaults={50,100,-50,.25F,1,2,2};break;
    case Type::Echo:defaults={50,50,500,500,0};break;
    case Type::Distortion:defaults={-18,15,2400,2400,8000};break;
    case Type::Compressor:defaults={0,10,200,-20,3,4};break;
    case Type::ParamEq:defaults={8000,12,0};break;
    case Type::I3DL2Reverb:s.count=12;s.words=preset_words(0);s.preset=0;break;
    case Type::WavesReverb:defaults={0,0,1000,.001F};break;
    }
    if(!defaults.empty()){s.count=static_cast<unsigned>(defaults.size());for(unsigned i=0;i<s.count;++i)s.words[i]=integer(type,i)?static_cast<std::uint32_t>(static_cast<std::int32_t>(defaults[i])):word(defaults[i]);}s.configure();
}
Effect::~Effect()=default;
Effect::Type Effect::type()const{return state_->type;}
std::span<const std::uint32_t> Effect::parameters()const{return std::span(state_->words).first(state_->count);}
bool Effect::set_parameters(std::span<const std::uint32_t> words){
    auto& s=*state_;if(words.size()!=s.count)return false;
    const auto valid=[&](unsigned i,float low,float high){const auto value=integer(s.type,i)?static_cast<float>(static_cast<std::int32_t>(words[i])):std::bit_cast<float>(words[i]);return std::isfinite(value) && value>=low && value<=high;};
    bool accepted=false;
    switch(s.type){
    case Type::Gargle:accepted=valid(0,1,1000) && valid(1,0,1);break;
    case Type::Chorus:case Type::Flanger:accepted=valid(0,0,100) && valid(1,0,100) && valid(2,-99,99) && valid(3,0,10) && valid(4,0,1) && valid(5,0,s.type==Type::Chorus?20.0F:4.0F) && valid(6,0,4);break;
    case Type::Echo:accepted=valid(0,0,100) && valid(1,0,100) && valid(2,1,2000) && valid(3,1,2000) && valid(4,0,1);break;
    case Type::Distortion:accepted=valid(0,-60,0) && valid(1,0,100) && valid(2,100,8000) && valid(3,100,8000) && valid(4,100,8000);break;
    case Type::Compressor:accepted=valid(0,-60,60) && valid(1,.01F,500) && valid(2,50,3000) && valid(3,-60,0) && valid(4,1,100) && valid(5,0,4);break;
    case Type::ParamEq:accepted=valid(0,80,16000) && valid(1,1,36) && valid(2,-15,15);break;
    case Type::I3DL2Reverb:accepted=valid(0,-10000,0) && valid(1,-10000,0) && valid(2,0,10) && valid(3,.1F,20) && valid(4,.1F,2) && valid(5,-10000,1000) && valid(6,0,.3F) && valid(7,-10000,2000) && valid(8,0,.1F) && valid(9,0,100) && valid(10,0,100) && valid(11,20,20000);break;
    case Type::WavesReverb:accepted=valid(0,-96,0) && valid(1,-96,0) && valid(2,.001F,3000) && valid(3,.001F,.999F);break;
    }
    if(!accepted && s.type==Type::I3DL2Reverb)for(unsigned i=0;i<std::size(presets);++i){const auto candidate=preset_words(i);if(std::equal(words.begin(),words.end(),candidate.begin())){accepted=true;break;}}
    if(!accepted)return false;std::copy(words.begin(),words.end(),s.words.begin());s.preset=UINT32_MAX;
    if(s.type==Type::I3DL2Reverb)for(unsigned i=0;i<std::size(presets);++i)if(preset_words(i)==s.words){s.preset=i;break;}s.configure();return true;
}
bool Effect::set_preset(unsigned preset){auto& s=*state_;if(s.type!=Type::I3DL2Reverb || preset>=std::size(presets))return false;s.words=preset_words(preset);s.preset=preset;s.configure();return true;}
unsigned Effect::preset()const{return state_->preset;}
bool Effect::set_quality(unsigned quality){auto& s=*state_;if(s.type!=Type::I3DL2Reverb || quality>3)return false;s.quality=quality;s.configure();return true;}
unsigned Effect::quality()const{return state_->quality;}
void Effect::reset(){auto& s=*state_;for(auto& channel:s.delay)std::fill(channel.begin(),channel.end(),0);for(auto& channel:s.comb)for(auto& line:channel){std::fill(line.data.begin(),line.data.end(),0);line.cursor=0;line.damp=0;}s.cursor=0;s.phase=0;s.low={};s.envelope={};s.tone={};for(auto& filter:s.eq)filter.z1=filter.z2=0;}
void Effect::process(float& left,float& right,float distance){
    auto& s=*state_;const std::array input{left,right};std::array output=input,stored=input;
    switch(s.type){
    case Type::Gargle:{const auto gain=s.p[1]==0?static_cast<float>(1-std::abs(2*s.phase-1)):(s.phase<.5?1.0F:0.0F);for(auto& sample:output)sample*=gain;s.advance_phase();
        left=std::isfinite(output[0])?std::clamp(output[0],-16.0F,16.0F):0;
        right=std::isfinite(output[1])?std::clamp(output[1],-16.0F,16.0F):0;return;}
    case Type::Chorus:case Type::Flanger:{
        for(unsigned c=0;c<2;++c){const auto phase=s.phase+(c?static_cast<double>(s.p[6]-2)/4:0);const auto lfo=s.p[4]==1?std::sin(2*pi*phase):1-4*std::abs(phase-std::floor(phase)-.5);const auto delay=s.p[5]*(1+s.p[1]/100*lfo)*s.rate/1000;const auto wet=delay<1?input[c]:s.delayed(c,delay);stored[c]=input[c]+wet*s.p[2]/100;output[c]=input[c]*(1-s.p[0]/100)+wet*s.p[0]/100;}
        s.advance_phase();break;}
    case Type::Echo:{const std::array wet{s.delayed(0,s.left_delay),s.delayed(1,s.right_delay)};for(unsigned c=0;c<2;++c){stored[c]=input[c]+wet[s.p[4]!=0?1-c:c]*s.p[1]/100;output[c]=input[c]*(1-s.p[0]/100)+wet[c]*s.p[0]/100;}break;}
    case Type::Distortion:for(unsigned c=0;c<2;++c){s.low[c]+=s.input_filter*(input[c]-s.low[c]);const auto edge=s.p[1]/100;const auto distorted=s.low[c]*(1-edge)+edge*std::tanh((1+edge*20)*s.low[c]);output[c]=s.eq[c].run(distorted)*s.output_gain;}break;
    case Type::Compressor:for(unsigned c=0;c<2;++c){const auto level=std::abs(input[c]),coefficient=level>s.envelope[c]?s.attack:s.release;s.envelope[c]=coefficient*s.envelope[c]+(1-coefficient)*level;const auto level_db=20*std::log10(std::max(s.envelope[c],1e-12F));const auto reduction=level_db>s.p[3]?(s.p[3]+(level_db-s.p[3])/s.p[4])-level_db:0;output[c]=(s.left_delay>=1?s.delayed(c,s.left_delay):input[c])*db(reduction)*s.output_gain;}break;
    case Type::ParamEq:for(unsigned c=0;c<2;++c)output[c]=s.eq[c].run(input[c]);break;
    case Type::I3DL2Reverb:case Type::WavesReverb:{
        const bool i3=s.type==Type::I3DL2Reverb;for(unsigned c=0;c<2;++c){s.low[c]+=s.input_filter*(input[c]-s.low[c]);stored[c]=(s.low[c]+(input[c]-s.low[c])*s.high_gain)*s.input_gain;
            const auto late=s.late_delay>=1?s.delayed(c,s.late_delay):stored[c];float wet=0;for(unsigned i=0;i<s.combs;++i){auto& line=s.comb[c][i];const auto sample=line.data[line.cursor];line.damp+=s.damping*(sample-line.damp);line.data[line.cursor]=std::clamp(late+line.damp*line.feedback+(sample-line.damp)*line.feedback_hf,-16.0F,16.0F);line.cursor=(line.cursor+1)%line.length;wet+=sample;}wet/=static_cast<float>(s.combs);
            wet=wet*s.diffusion+(s.delayed(c,static_cast<double>(s.rate)*.03F))*(1-s.diffusion);const auto early=s.early_delay>=1?s.delayed(c,s.early_delay):stored[c];const auto rolloff=i3?1/(1+s.p[2]*std::max(distance,0.0F)):1;output[c]=input[c]*(i3?1:s.input_gain)+(early*s.early_gain+wet*s.wet_gain)*rolloff;}
        break;}
    }
    for(unsigned c=0;c<2;++c){s.delay[c][s.cursor]=std::isfinite(stored[c])?std::clamp(stored[c],-16.0F,16.0F):0;output[c]=std::isfinite(output[c])?std::clamp(output[c],-16.0F,16.0F):0;}s.cursor=(s.cursor+1)%s.delay[0].size();left=output[0];right=output[1];
}
}
