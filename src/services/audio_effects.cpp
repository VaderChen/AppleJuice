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
bool integer(Effect::Type type,unsigned index){return type==Effect::Type::Gargle || type==Effect::Type::FxMasteringLimiter || ((type==Effect::Type::Chorus || type==Effect::Type::Flanger) && (index==4 || index==6)) || (type==Effect::Type::Echo && index==4) || (type==Effect::Type::I3DL2Reverb && (index==0 || index==1 || index==5 || index==7));}
// XAPOFX 參數的最小值、最大值與預設值（xapofx.h 的 FXEQ_／FXMASTERINGLIMITER_／FXREVERB_／FXECHO_ 常數），順序同參數結構。
struct FxRange {float low,high,fallback;};
constexpr FxRange fx_eq[]{{20,20000,100},{.126F,7.94F,1},{.1F,2,1},{20,20000,800},{.126F,7.94F,1},{.1F,2,1},{20,20000,2000},{.126F,7.94F,1},{.1F,2,1},{20,20000,10000},{.126F,7.94F,1},{.1F,2,1}};
constexpr FxRange fx_limiter[]{{1,20,6},{1,1800,1000}};
constexpr FxRange fx_reverb[]{{0,1,.9F},{.0001F,1,.6F}};
constexpr FxRange fx_echo[]{{0,1,.5F},{0,1,.5F},{1,2000,500}};
std::span<const FxRange> fx_ranges(Effect::Type type){
    switch(type){case Effect::Type::FxEq:return fx_eq;case Effect::Type::FxMasteringLimiter:return fx_limiter;case Effect::Type::FxReverb:return fx_reverb;case Effect::Type::FxEcho:return fx_echo;default:return {};}
}
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
    // XAPOFX：四段等化器每聲道四個串接 biquad；殘響在梳狀濾波器後串接兩級全通（只用 data／cursor／length）。
    std::array<std::array<Biquad,4>,2> bands;
    std::array<std::array<Comb,2>,2> allpass;
    unsigned combs{6};float damping{},input_filter{},early_gain{},wet_gain{},input_gain{1},high_gain{1},diffusion{1},attack{},release{},output_gain{1};
    double left_delay{},right_delay{},early_delay{},late_delay{};
    double phase_step{};
    State(Type t,unsigned r):type(t),rate(r){
        if(r<100 || r>200000 || static_cast<unsigned>(t)>static_cast<unsigned>(Type::FxEcho))throw std::invalid_argument("音訊效果種類或取樣率無效");
        // Gargle 不經過延遲流程；另外兩種不讀延遲的效果只保留共用單槽。
        const auto frames=t==Type::Gargle?std::size_t{0}:
            t==Type::Distortion || t==Type::ParamEq || t==Type::FxEq || t==Type::FxMasteringLimiter || t==Type::FxReverb?std::size_t{1}:static_cast<std::size_t>(r)*2+8;
        for(auto& channel:delay)channel.resize(frames);
        if(t==Type::I3DL2Reverb || t==Type::WavesReverb || t==Type::FxReverb)for(auto& channel:comb)for(auto& line:channel)line.data.resize(static_cast<std::size_t>(r)/4+8);
        if(t==Type::FxReverb)for(auto& channel:allpass)for(auto& line:channel)line.data.resize(static_cast<std::size_t>(r)/50+8);
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
        }else if(type==Type::FxEq){
            // RBJ peaking：Gain 是中心頻率的線性振幅增益（A²），Bandwidth 以八度計。
            // 係數存成「與直通的差」：H=1+D/A，D=(b−a)。Gain=1 時 D 全為零、狀態維持零，輸出與輸入逐位元相同（直接式會留下捨入殘差）。
            for(unsigned band=0;band<4;++band){
                const auto w=2*pi*std::min(static_cast<double>(p[band*3]),rate*.45)/rate;const auto sine=std::sin(w),cosine=std::cos(w);
                const auto a=std::sqrt(static_cast<double>(p[band*3+1]));const auto alpha=sine*std::sinh(std::log(2.0)*.5*p[band*3+2]*w/sine);const auto divisor=1+alpha/a;
                const auto difference=(alpha*a-alpha/a)/divisor;
                for(auto& channel:bands){auto& f=channel[band];f.b0=difference;f.b1=0;f.b2=-difference;f.a1=-2*cosine/divisor;f.a2=(1-alpha/a)/divisor;}
            }
        }else if(type==Type::FxMasteringLimiter){
            // 標頭只定義範圍，沒有公開單位。此處：Loudness/1000 為前級增益（預設 1000＝不變），上限固定 0 dBFS；Release 每單位 10 ms 的釋放時間常數。
            output_gain=p[1]/1000;release=static_cast<float>(std::exp(-1.0/(p[0]*.01*static_cast<double>(rate))));
        }else if(type==Type::FxReverb){
            // RoomSize 同時拉長梳狀延遲與衰減時間（0.2～3 秒）；Diffusion 是全通係數，0 時全通退化為純延遲。
            const double room=p[1],decay=.2+2.8*room;combs=8;damping=filter(5000);diffusion=.7F*p[0];wet_gain=.3F;
            constexpr std::array lengths{.0297,.0371,.0411,.0437,.0531,.0617,.0719,.0793};constexpr std::array diffusers{.005,.0017};
            for(unsigned c=0;c<2;++c){
                for(unsigned i=0;i<8;++i){auto& line=comb[c][i];line.length=std::clamp<std::size_t>(static_cast<std::size_t>(rate*(lengths[i]*(.25+.75*room)+c*.0013)),1,line.data.size());line.cursor%=line.length;const auto seconds=static_cast<double>(line.length)/rate;line.feedback=static_cast<float>(std::pow(.001,seconds/decay));line.feedback_hf=static_cast<float>(std::pow(.001,seconds/(decay*.5)));}
                for(unsigned i=0;i<2;++i){auto& line=allpass[c][i];line.length=std::clamp<std::size_t>(static_cast<std::size_t>(rate*(diffusers[i]+c*.0003)),1,line.data.size());line.cursor%=line.length;}
            }
        }else if(type==Type::FxEcho){left_delay=right_delay=p[2]*static_cast<double>(rate)/1000.0;
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
    // 一個聲道的梳狀濾波器組：高頻另以較短的衰減回授，回傳各線輸出的平均。
    float run_combs(unsigned channel,float input){
        float wet=0;
        for(unsigned i=0;i<combs;++i){auto& line=comb[channel][i];const auto sample=line.data[line.cursor];line.damp+=damping*(sample-line.damp);line.data[line.cursor]=std::clamp(input+line.damp*line.feedback+(sample-line.damp)*line.feedback_hf,-16.0F,16.0F);line.cursor=(line.cursor+1)%line.length;wet+=sample;}
        return wet/static_cast<float>(combs);
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
    case Type::FxEq:case Type::FxMasteringLimiter:case Type::FxReverb:case Type::FxEcho:for(const auto& range:fx_ranges(type))defaults.push_back(range.fallback);break;
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
    case Type::FxEq:case Type::FxMasteringLimiter:case Type::FxReverb:case Type::FxEcho:{const auto ranges=fx_ranges(s.type);accepted=true;for(unsigned i=0;i<ranges.size();++i)accepted=accepted && valid(i,ranges[i].low,ranges[i].high);break;}
    }
    if(!accepted && s.type==Type::I3DL2Reverb)for(unsigned i=0;i<std::size(presets);++i){const auto candidate=preset_words(i);if(std::equal(words.begin(),words.end(),candidate.begin())){accepted=true;break;}}
    if(!accepted)return false;std::copy(words.begin(),words.end(),s.words.begin());s.preset=UINT32_MAX;
    if(s.type==Type::I3DL2Reverb)for(unsigned i=0;i<std::size(presets);++i)if(preset_words(i)==s.words){s.preset=i;break;}s.configure();return true;
}
bool Effect::set_parameters_clamped(std::span<const std::uint32_t> words){
    auto& s=*state_;const auto ranges=fx_ranges(s.type);if(ranges.empty() || words.size()!=s.count)return set_parameters(words);
    std::array<std::uint32_t,12> clamped{};
    for(unsigned i=0;i<s.count;++i){
        // 限幅器欄位是 UINT32：以無號值夾範圍，過大的值落在上限而非被當成負數。
        if(integer(s.type,i))clamped[i]=static_cast<std::uint32_t>(std::clamp(static_cast<double>(words[i]),static_cast<double>(ranges[i].low),static_cast<double>(ranges[i].high)));
        else {const auto value=std::bit_cast<float>(words[i]);clamped[i]=word(std::isfinite(value)?std::clamp(value,ranges[i].low,ranges[i].high):ranges[i].fallback);}
    }
    return set_parameters(std::span(clamped).first(s.count));
}
bool Effect::set_preset(unsigned preset){auto& s=*state_;if(s.type!=Type::I3DL2Reverb || preset>=std::size(presets))return false;s.words=preset_words(preset);s.preset=preset;s.configure();return true;}
unsigned Effect::preset()const{return state_->preset;}
bool Effect::set_quality(unsigned quality){auto& s=*state_;if(s.type!=Type::I3DL2Reverb || quality>3)return false;s.quality=quality;s.configure();return true;}
unsigned Effect::quality()const{return state_->quality;}
void Effect::reset(){auto& s=*state_;for(auto& channel:s.delay)std::fill(channel.begin(),channel.end(),0);for(auto& channel:s.comb)for(auto& line:channel){std::fill(line.data.begin(),line.data.end(),0);line.cursor=0;line.damp=0;}s.cursor=0;s.phase=0;s.low={};s.envelope={};s.tone={};for(auto& filter:s.eq)filter.z1=filter.z2=0;
    for(auto& channel:s.bands)for(auto& filter:channel)filter.z1=filter.z2=0;for(auto& channel:s.allpass)for(auto& line:channel){std::fill(line.data.begin(),line.data.end(),0);line.cursor=0;}}
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
            const auto late=s.late_delay>=1?s.delayed(c,s.late_delay):stored[c];float wet=s.run_combs(c,late);
            wet=wet*s.diffusion+(s.delayed(c,static_cast<double>(s.rate)*.03F))*(1-s.diffusion);const auto early=s.early_delay>=1?s.delayed(c,s.early_delay):stored[c];const auto rolloff=i3?1/(1+s.p[2]*std::max(distance,0.0F)):1;output[c]=input[c]*(i3?1:s.input_gain)+(early*s.early_gain+wet*s.wet_gain)*rolloff;}
        break;}
    case Type::FxEq:for(unsigned c=0;c<2;++c)for(auto& filter:s.bands[c])output[c]+=filter.run(output[c]);break;
    case Type::FxMasteringLimiter:{
        // 兩聲道共用增益；包絡立即跟上峰值、依 Release 指數回落，包絡不低於當下峰值，所以輸出不超過 0 dBFS。
        auto peak=std::max(std::abs(input[0]),std::abs(input[1]))*s.output_gain;if(!std::isfinite(peak))peak=0;
        s.envelope[0]=peak>s.envelope[0]?peak:peak+(s.envelope[0]-peak)*s.release;const auto gain=s.envelope[0]>1?s.output_gain/s.envelope[0]:s.output_gain;
        for(unsigned c=0;c<2;++c)output[c]=std::clamp(input[c]*gain,-1.0F,1.0F);break;}
    case Type::FxReverb:for(unsigned c=0;c<2;++c){
        auto wet=s.run_combs(c,input[c]);
        for(auto& line:s.allpass[c]){const auto delayed=line.data[line.cursor];const auto feed=std::clamp(wet+delayed*s.diffusion,-16.0F,16.0F);line.data[line.cursor]=feed;line.cursor=(line.cursor+1)%line.length;wet=delayed-feed*s.diffusion;}
        output[c]=input[c]+wet*s.wet_gain;}break;
    case Type::FxEcho:for(unsigned c=0;c<2;++c){const auto wet=s.delayed(c,s.left_delay);stored[c]=input[c]+wet*s.p[1];output[c]=input[c]*(1-s.p[0])+wet*s.p[0];}break;
    }
    for(unsigned c=0;c<2;++c){s.delay[c][s.cursor]=std::isfinite(stored[c])?std::clamp(stored[c],-16.0F,16.0F):0;output[c]=std::isfinite(output[c])?std::clamp(output[c],-16.0F,16.0F):0;}s.cursor=(s.cursor+1)%s.delay[0].size();left=output[0];right=output[1];
}
}
