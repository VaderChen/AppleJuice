#pragma once
namespace x64arm::d3dx_animation {
using Vec=std::array<float,3>;using Quat=std::array<float,4>;using Mat=std::array<float,16>;
struct Pose {Vec scale{1,1,1},translation{};Quat rotation{0,0,0,1};};
inline Quat normalized(Quat value){double length=0;for(auto c:value)length+=double(c)*c;if(length>0){const auto inv=1/std::sqrt(length);for(auto& c:value)c=static_cast<float>(c*inv);}else value={0,0,0,1};return value;}
inline Quat slerp(Quat a,Quat b,double t){a=normalized(a);b=normalized(b);double dot=0;for(unsigned c=0;c<4;++c)dot+=double(a[c])*b[c];if(dot<0){for(auto& c:b)c=-c;dot=-dot;}double left=1-t,right=t;if(dot<.9995){const auto angle=std::acos(std::clamp(dot,-1.,1.)),denominator=std::sin(angle);left=std::sin((1-t)*angle)/denominator;right=std::sin(t*angle)/denominator;}Quat value{};for(unsigned c=0;c<4;++c)value[c]=static_cast<float>(left*a[c]+right*b[c]);return normalized(value);}
inline Pose interpolate(const Pose& a,const Pose& b,double t){Pose value;for(unsigned c=0;c<3;++c){value.scale[c]=static_cast<float>(a.scale[c]+t*(b.scale[c]-a.scale[c]));value.translation[c]=static_cast<float>(a.translation[c]+t*(b.translation[c]-a.translation[c]));}value.rotation=slerp(a.rotation,b.rotation,t);return value;}
inline Mat compose(const Pose& pose){const auto q=normalized(pose.rotation);const float x=q[0],y=q[1],z=q[2],w=q[3];Mat value{1-2*y*y-2*z*z,2*x*y+2*z*w,2*x*z-2*y*w,0,2*x*y-2*z*w,1-2*x*x-2*z*z,2*y*z+2*x*w,0,2*x*z+2*y*w,2*y*z-2*x*w,1-2*x*x-2*y*y,0,pose.translation[0],pose.translation[1],pose.translation[2],1};for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)value[r*4+c]*=pose.scale[r];return value;}
inline Pose decompose(const Mat& source){
    Pose pose;Mat m=source;for(unsigned r=0;r<3;++r){double sum=0;for(unsigned c=0;c<3;++c)sum+=double(m[r*4+c])*m[r*4+c];pose.scale[r]=static_cast<float>(std::sqrt(sum));pose.translation[r]=m[12+r];}
    const double determinant=double(m[0])*(double(m[5])*m[10]-double(m[6])*m[9])-double(m[1])*(double(m[4])*m[10]-double(m[6])*m[8])+double(m[2])*(double(m[4])*m[9]-double(m[5])*m[8]);if(determinant<0)pose.scale[0]=-pose.scale[0];
    for(unsigned r=0;r<3;++r){if(pose.scale[r]==0)throw std::invalid_argument("動畫矩陣含退化縮放");for(unsigned c=0;c<3;++c)m[r*4+c]/=pose.scale[r];}
    const auto trace=m[0]+m[5]+m[10];auto& q=pose.rotation;
    if(trace>0){const auto s=std::sqrt(trace+1)*2;q={(m[6]-m[9])/s,(m[8]-m[2])/s,(m[1]-m[4])/s,s*.25F};}
    else {unsigned i=0;if(m[5]>m[0])i=1;if(m[10]>m[i*4+i])i=2;const auto j=(i+1)%3,k=(i+2)%3;const auto s=std::sqrt(std::max(0.F,1+m[i*4+i]-m[j*4+j]-m[k*4+k]))*2;if(s==0)throw std::invalid_argument("動畫矩陣無法分解旋轉");q[i]=s*.25F;q[j]=(m[i*4+j]+m[j*4+i])/s;q[k]=(m[i*4+k]+m[k*4+i])/s;q[3]=(m[j*4+k]-m[k*4+j])/s;}
    q=normalized(q);return pose;
}
struct Key {float time{};Quat value{};};
struct Animation {std::string name;std::array<std::vector<Key>,3> keys;std::uint64_t guest_name{};};
struct Callback {float time{};std::uint64_t data{};};
struct Set {
    std::string name;double ticks{4800};unsigned playback{},capacity{};std::uint64_t guest_name{};std::vector<Animation> animations;std::vector<Callback> callbacks;std::vector<std::uint8_t> compressed;
    double tick_period()const{double end=0;for(const auto& animation:animations)for(const auto& keys:animation.keys)if(!keys.empty())end=std::max(end,double(keys.back().time));return end;}
    double period()const{return tick_period()/ticks;}
    double periodic(double position)const{const auto length=period();if(length<=0)return 0;if(playback==1)return std::clamp(position,0.,length);const auto cycle=length*(playback==2?2:1);auto value=std::fmod(position,cycle);if(value<0)value+=cycle;return playback==2 && value>length?cycle-value:value;}
    Pose sample(unsigned index,double seconds)const{
        const auto& animation=animations.at(index);Pose out;const auto position=periodic(seconds)*ticks;
        for(unsigned channel=0;channel<3;++channel){const auto& keys=animation.keys[channel];if(keys.empty())continue;const auto after=std::upper_bound(keys.begin(),keys.end(),position,[](double time,const Key& key){return time<key.time;});const auto& a=after==keys.begin()?keys.front():*std::prev(after);const auto& b=after==keys.end()?keys.back():*after;const auto t=b.time>a.time?std::clamp((position-a.time)/(b.time-a.time),0.,1.):0.;Quat value{};
            if(channel==1)value=slerp(a.value,b.value,t);else for(unsigned c=0;c<3;++c)value[c]=static_cast<float>(a.value[c]+t*(b.value[c]-a.value[c]));
            if(channel==1)out.rotation=value;else for(unsigned c=0;c<3;++c)(channel==0?out.scale:out.translation)[c]=value[c];
        }return out;
    }
};
inline Set from_x(const d3d9_xfile::AnimationSet& source,double ticks){
    Set value;value.name=source.name;value.ticks=ticks;value.capacity=static_cast<unsigned>(source.animations.size());
    for(const auto& original:source.animations){Animation animation;animation.name=original.name;
        for(const auto& [type,keys]:original.keys)for(const auto& key:keys){const auto time=static_cast<float>(key.time);
            if(type>=3){Mat matrix{};std::copy_n(key.values.begin(),16,matrix.begin());const auto pose=decompose(matrix);animation.keys[0].push_back({time,{pose.scale[0],pose.scale[1],pose.scale[2],0}});animation.keys[1].push_back({time,pose.rotation});animation.keys[2].push_back({time,{pose.translation[0],pose.translation[1],pose.translation[2],0}});}
            // X-file 的 scalar-first 旋轉鍵與 D3DX row-vector 四元數方向相反，載入時轉共軛。
            else if(type==0)animation.keys[1].push_back({time,{-key.values[1],-key.values[2],-key.values[3],key.values[0]}});
            else animation.keys[type==1?0:2].push_back({time,{key.values[0],key.values[1],key.values[2],0}});
        }
        for(auto& keys:animation.keys)std::stable_sort(keys.begin(),keys.end(),[](const auto& a,const auto& b){return a.time<b.time;});value.animations.push_back(std::move(animation));
    }return value;
}
struct Output {std::string name;std::array<std::uint64_t,4> pointers{};Pose initial;};
struct Track {std::uint64_t set{};unsigned priority{};float weight{1},speed{1};double position{};bool enabled{true};};
struct Event {unsigned id{},type{},track{},transition{};double start{},duration{},target{},initial{};bool started{},finished{};};
struct Controller {unsigned maximum_outputs{},maximum_sets{},maximum_events{},next_event{};double time{};float blend{};std::vector<std::uint64_t> sets;std::vector<Track> tracks;std::vector<Output> outputs;std::vector<Event> events;};
}
