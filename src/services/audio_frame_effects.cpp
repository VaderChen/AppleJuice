#include "x64arm/audio_frame_effects.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace x64arm::audio {
namespace {
constexpr float reference_rate=48000; // xaudio2fx.h：時間與頻率參數都相對於 48 kHz
constexpr unsigned input_line=(300+85)*48+64;  // 最長 ReflectionsDelay＋ReverbDelay，再加早期反射散布
constexpr unsigned comb_count=8;
// 晚期殘響的梳狀濾波器與擴散全通延遲（48 kHz 樣本數，互質以避免共振重合）。
constexpr std::array<unsigned,comb_count> comb_delays{1215,1293,1390,1476,1548,1623,1695,1760};
constexpr std::array<unsigned,2> late_allpass{605,371},early_allpass{142,107};
// 各輸出聲道從梳狀濾波器取用的符號（彼此正交以去相關）。
constexpr std::array<std::array<signed char,comb_count>,6> output_signs{{
    {{1,-1,1,-1,1,-1,1,-1}},{{1,1,-1,-1,1,1,-1,-1}},{{1,1,1,1,-1,-1,-1,-1}},
    {{1,-1,-1,1,1,-1,-1,1}},{{1,-1,1,-1,-1,1,-1,1}},{{1,1,-1,-1,-1,-1,1,1}}}};
float db(float value){return std::pow(10.F,value/20.F);}
float one_pole(float frequency){return 1-std::exp(-6.2831853F*std::clamp(frequency,1.F,23900.F)/reference_rate);}
template<class T>T fit(T value,T low,T high){return std::clamp(value,low,high);}
float fitf(float value,float low,float high,float fallback){return std::isfinite(value)?std::clamp(value,low,high):fallback;}
struct Allpass {
    std::vector<float> data;unsigned at{};
    float process(float input,float gain){
        const float delayed=data[at];const float stored=input+delayed*gain;data[at]=stored;at=(at+1)%static_cast<unsigned>(data.size());return delayed-stored*gain;
    }
};
struct Delay {
    std::vector<float> data;unsigned at{};
    void push(float value){data[at]=value;at=(at+1)%static_cast<unsigned>(data.size());}
    float tap(unsigned delay)const {const auto size=static_cast<unsigned>(data.size());delay=std::min(delay,size-1);return data[(at+size-1-delay)%size];}
};
}
ReverbParameters clamp_reverb(const ReverbParameters& p,bool seven_one){
    const ReverbParameters d{};ReverbParameters r=p;
    r.wet_dry_mix=fitf(p.wet_dry_mix,0,100,d.wet_dry_mix);
    r.reflections_delay=fit<std::uint32_t>(p.reflections_delay,0,300);
    r.reverb_delay=fit<std::uint8_t>(p.reverb_delay,0,85);
    r.rear_delay=fit<std::uint8_t>(p.rear_delay,0,seven_one?20:5);
    r.side_delay=fit<std::uint8_t>(p.side_delay,0,5);
    for(auto* v:{&r.position_left,&r.position_right,&r.position_matrix_left,&r.position_matrix_right})*v=fit<std::uint8_t>(*v,0,30);
    r.early_diffusion=fit<std::uint8_t>(p.early_diffusion,0,15);r.late_diffusion=fit<std::uint8_t>(p.late_diffusion,0,15);
    r.low_eq_gain=fit<std::uint8_t>(p.low_eq_gain,0,12);r.low_eq_cutoff=fit<std::uint8_t>(p.low_eq_cutoff,0,9);
    r.high_eq_gain=fit<std::uint8_t>(p.high_eq_gain,0,8);r.high_eq_cutoff=fit<std::uint8_t>(p.high_eq_cutoff,0,14);
    r.room_filter_freq=fitf(p.room_filter_freq,20,20000,d.room_filter_freq);
    r.room_filter_main=fitf(p.room_filter_main,-100,0,d.room_filter_main);
    r.room_filter_hf=fitf(p.room_filter_hf,-100,0,d.room_filter_hf);
    r.reflections_gain=fitf(p.reflections_gain,-100,20,d.reflections_gain);
    r.reverb_gain=fitf(p.reverb_gain,-100,20,d.reverb_gain);
    r.decay_time=std::isnan(p.decay_time)?d.decay_time:std::max(p.decay_time,.1F);
    r.density=fitf(p.density,0,100,d.density);
    r.room_size=fitf(p.room_size,1,100,d.room_size);
    return r;
}
struct Reverb::State {
    std::mutex mutex;std::atomic<bool> dirty{};ReverbParameters pending,current;
    // 由參數換算的係數。
    float mix{},early_gain{},late_gain{},room_main{},room_hf{},room_coefficient{},early_diffusion{},late_diffusion{};
    float low_coefficient{},high_coefficient{};
    std::array<float,comb_count> g_low{},g_mid{},g_high{},comb_input{};
    std::array<unsigned,comb_count> length{};
    unsigned active{comb_count},early_tap{},late_tap{},rear_delay{},side_delay{};
    std::array<unsigned,4> early_offsets{};
    std::array<float,4> position{}; // 前左、前右、矩陣左、矩陣右（0 近～1 遠）
    // 狀態
    std::array<Delay,2> input;std::array<float,2> room{};
    std::array<std::array<Allpass,2>,2> early;std::array<Allpass,2> late;
    std::array<std::vector<float>,comb_count> comb;std::array<unsigned,comb_count> comb_at{};
    std::array<float,comb_count> damp_low{},damp_high{};
    std::array<Delay,4> surround; // 後左、後右、側左、側右
    State(){
        for(auto& line:input)line.data.assign(input_line,0);
        for(unsigned c=0;c<2;++c)for(unsigned i=0;i<2;++i)early[c][i].data.assign(early_allpass[i]+c*7,0);
        for(unsigned i=0;i<2;++i)late[i].data.assign(late_allpass[i],0);
        for(unsigned i=0;i<comb_count;++i)comb[i].assign(comb_delays[i]+1,0);
        for(auto& line:surround)line.data.assign(20*48+2,0);
        derive();
    }
    void derive(){
        const auto& p=current;mix=p.wet_dry_mix/100;
        early_gain=db(p.reflections_gain);room_main=db(p.room_filter_main);room_hf=db(p.room_filter_hf);room_coefficient=one_pole(p.room_filter_freq);
        early_diffusion=.04F*p.early_diffusion;late_diffusion=.15F+.035F*p.late_diffusion;
        early_tap=p.reflections_delay*48;late_tap=(p.reflections_delay+p.reverb_delay)*48;rear_delay=p.rear_delay*48U;side_delay=p.side_delay*48U;
        // 早期反射：第一道在 ReflectionsDelay，後續間距隨房間大小。
        const float spacing=(3+12*p.room_size/100)*48;
        constexpr std::array<float,4> steps{0,1,2.3F,3.7F};
        for(unsigned i=0;i<4;++i)early_offsets[i]=early_tap+static_cast<unsigned>(steps[i]*spacing);
        // Density 控制晚期場的模態密度：密度低時梳狀濾波器少、聲音空洞。
        active=std::clamp(2U+static_cast<unsigned>(std::lround(6*p.density/100)),2U,comb_count);
        const float scale=.2F+.8F*(p.room_size-1)/99;
        low_coefficient=one_pole(50.F+50.F*p.low_eq_cutoff);high_coefficient=one_pole(1000.F+500.F*p.high_eq_cutoff);
        // LowEQGain 8 與 HighEQGain 8 為 0 dB（與 1 kHz 衰減時間相同）。
        const float low_time=p.decay_time*db(static_cast<float>(p.low_eq_gain)-8),high_time=p.decay_time*db(static_cast<float>(p.high_eq_gain)-8);
        for(unsigned i=0;i<comb_count;++i){
            length[i]=std::max(48U,static_cast<unsigned>(static_cast<float>(comb_delays[i])*scale));
            const float seconds=static_cast<float>(length[i])/reference_rate;
            const auto gain=[&](float time){return std::isinf(time)?1.F:std::pow(10.F,-3*seconds/std::max(time,.01F));};
            g_low[i]=gain(low_time);g_mid[i]=gain(p.decay_time);g_high[i]=gain(high_time);
            // 帶分離迴授的上界：不超過 0.999，避免長殘響時發散。
            const float bound=g_high[i]+std::abs(g_mid[i]-g_high[i])+std::abs(g_low[i]-g_mid[i]);
            if(bound>.999F){const float k=.999F/bound;g_low[i]*=k;g_mid[i]*=k;g_high[i]*=k;}
            comb_input[i]=std::sqrt(std::max(1-g_mid[i]*g_mid[i],1e-6F));
        }
        late_gain=p.disable_late_field?0:db(p.reverb_gain)/std::sqrt(static_cast<float>(active));
        position={p.position_left/30.F,p.position_right/30.F,p.position_matrix_left/30.F,p.position_matrix_right/30.F};
    }
    void clear(){
        for(auto& line:input)std::fill(line.data.begin(),line.data.end(),0.F);room.fill(0);
        for(auto& side:early)for(auto& a:side)std::fill(a.data.begin(),a.data.end(),0.F);
        for(auto& a:late)std::fill(a.data.begin(),a.data.end(),0.F);
        for(auto& c:comb)std::fill(c.begin(),c.end(),0.F);comb_at.fill(0);damp_low.fill(0);damp_high.fill(0);
        for(auto& line:surround)std::fill(line.data.begin(),line.data.end(),0.F);
    }
};
Reverb::Reverb(bool surround71):state_(std::make_unique<State>()),surround71_(surround71){input_=output_=2;}
Reverb::~Reverb()=default;
bool Reverb::supports(unsigned in,unsigned out,bool surround71){
    return (in==1 || in==2) && (out==in || out==6 || (surround71 && out==8));
}
void Reverb::set(const ReverbParameters& value){std::lock_guard lock(state_->mutex);state_->pending=clamp_reverb(value,output_==8);state_->dirty.store(true,std::memory_order_release);}
ReverbParameters Reverb::parameters()const {std::lock_guard lock(state_->mutex);return state_->dirty.load(std::memory_order_acquire)?state_->pending:state_->current;}
bool Reverb::configure(unsigned rate,unsigned in,unsigned out){
    if(!supports(in,out,surround71_) || rate<20000 || rate>48000)return false;
    std::lock_guard lock(state_->mutex);rate_=rate;input_=in;output_=out;
    state_->current=clamp_reverb(state_->dirty.exchange(false)?state_->pending:state_->current,out==8);state_->derive();state_->clear();return true;
}
void Reverb::reset(){std::lock_guard lock(state_->mutex);state_->clear();}
void Reverb::process(float* frame,bool enabled){
    auto& s=*state_;
    // 與參數更新、重新設定共用同一把鎖；混音執行緒持鎖期間只處理這一幀。
    std::lock_guard lock(s.mutex);
    if(s.dirty.load(std::memory_order_relaxed)){s.current=s.pending;s.dirty.store(false,std::memory_order_relaxed);s.derive();}
    const float x0=frame[0],x1=input_==2?frame[1]:frame[0];
    std::array<float,8> out{};
    if(!enabled){
        // XAPO 直通：只做聲道格式轉換（單聲道到多聲道放到前左右）。
        out[0]=x0;if(output_>=2)out[1]=x1;
        for(unsigned c=0;c<output_;++c)frame[c]=out[c];
        return;
    }
    // 房間低通（主增益＋轉角高頻衰減）作用於早期反射與晚期場。
    const std::array<float,2> dry{x0,x1};std::array<float,2> wet_in{};
    for(unsigned c=0;c<input_;++c){s.room[c]+=s.room_coefficient*(dry[c]-s.room[c]);wet_in[c]=s.room_main*(s.room[c]+s.room_hf*(dry[c]-s.room[c]));s.input[c].push(wet_in[c]);}
    if(input_==1)s.input[1].push(wet_in[0]);
    // 早期反射：四個隨房間大小散布的反射，再經 EarlyDiffusion 全通擴散。
    std::array<float,2> early{};
    constexpr std::array<float,4> tap_gain{.66F,.53F,.42F,.33F};
    for(unsigned c=0;c<2;++c){float sum=0;for(unsigned i=0;i<4;++i)sum+=s.input[c].tap(s.early_offsets[i]+c*29)*tap_gain[i];
        for(auto& a:s.early[c])sum=a.process(sum,s.early_diffusion);early[c]=sum*s.early_gain;}
    // 晚期場：延後 ReverbDelay 的單聲道輸入經 LateDiffusion 擴散後進入梳狀濾波器組。
    std::array<float,6> late{};
    if(s.late_gain>0){
        float feed=.5F*(s.input[0].tap(s.late_tap)+s.input[1].tap(s.late_tap));
        for(auto& a:s.late)feed=a.process(feed,s.late_diffusion);
        std::array<float,comb_count> taps{};
        for(unsigned i=0;i<s.active;++i){
            auto& line=s.comb[i];const auto length=s.length[i];auto& at=s.comb_at[i];if(at>=length)at=0;
            const float y=line[at];
            // 依 LowEQ／HighEQ 轉角把迴授分成三段，各自以對應衰減時間的增益回授。
            s.damp_low[i]+=s.low_coefficient*(y-s.damp_low[i]);s.damp_high[i]+=s.high_coefficient*(y-s.damp_high[i]);
            const float low=s.damp_low[i],high=y-s.damp_high[i],mid=y-low-high;
            line[at]=feed*s.comb_input[i]+s.g_low[i]*low+s.g_mid[i]*mid+s.g_high[i]*high;at=(at+1)%length;taps[i]=y;
        }
        for(unsigned o=0;o<6;++o){float sum=0;for(unsigned i=0;i<s.active;++i)sum+=taps[i]*static_cast<float>(output_signs[o][i]);late[o]=sum*s.late_gain;}
    }
    // Position：0 近（早期反射為主、殘響退後減弱）～30 遠；不改變衰減時間。
    const auto place=[&](float e,float l,float p){return e*(1-.5F*p)+l*(.5F+.5F*p);};
    const float m=s.mix,keep=1-m;
    out[0]=keep*x0+m*place(early[0],late[0],s.position[0]);
    if(output_==2 || output_>=6)out[1]=keep*x1+m*place(early[1],late[1],input_==1?s.position[0]:s.position[1]);
    if(output_>=6){
        // 5.1／7.1：中央與 LFE 不放殘響；後方（及 7.1 側邊）以矩陣位置與 RearDelay／SideDelay 放置晚期場。
        s.surround[0].push(m*place(.5F*early[0],late[2],s.position[2]));
        s.surround[1].push(m*place(.5F*early[1],late[3],input_==1?s.position[2]:s.position[3]));
        out[4]=s.surround[0].tap(s.rear_delay);out[5]=s.surround[1].tap(s.rear_delay);
        if(output_==8){
            s.surround[2].push(m*place(.5F*early[0],late[4],s.position[2]));
            s.surround[3].push(m*place(.5F*early[1],late[5],input_==1?s.position[2]:s.position[3]));
            out[6]=s.surround[2].tap(s.side_delay);out[7]=s.surround[3].tap(s.side_delay);
        }
    }
    for(unsigned c=0;c<output_;++c)frame[c]=std::isfinite(out[c])?out[c]:0.F;
}
bool VolumeMeter::configure(unsigned rate,unsigned in,unsigned out){
    if(!in || in!=out || in>64 || rate<1000)return false;
    std::lock_guard lock(mutex_);rate_=rate;input_=output_=in;pass_=std::max(1U,rate/100);
    peak_.assign(in,0);sum_.assign(in,0);published_peak_.assign(in,0);published_rms_.assign(in,0);count_=0;return true;
}
void VolumeMeter::reset(){std::lock_guard lock(mutex_);std::fill(peak_.begin(),peak_.end(),0.F);std::fill(sum_.begin(),sum_.end(),0.F);count_=0;}
void VolumeMeter::process(float* frame,bool enabled){
    std::lock_guard lock(mutex_);
    if(!enabled || peak_.size()!=input_)return;
    for(unsigned c=0;c<input_;++c){const float v=frame[c];if(!std::isfinite(v))continue;peak_[c]=std::max(peak_[c],std::abs(v));sum_[c]+=v*v;}
    if(++count_>=pass_)publish();
}
void VolumeMeter::publish(){
    if(!count_)return;
    for(unsigned c=0;c<peak_.size();++c){published_peak_[c]=peak_[c];published_rms_[c]=std::sqrt(sum_[c]/static_cast<float>(count_));peak_[c]=sum_[c]=0;}
    count_=0;
}
void VolumeMeter::finish_pass(){std::lock_guard lock(mutex_);publish();}
void VolumeMeter::set_pass(unsigned frames){std::lock_guard lock(mutex_);pass_=std::max(1U,frames);}
VolumeMeter::Levels VolumeMeter::levels()const {std::lock_guard lock(mutex_);return {published_peak_,published_rms_};}
}
