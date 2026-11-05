#include "x64arm/audio_graph.hpp"
#include "x64arm/audio_pcm.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <stdexcept>

namespace x64arm::audio {
namespace {
constexpr unsigned maximum_channels=64,maximum_packets=64;
void require(bool valid){if(!valid)throw std::invalid_argument("音訊圖參數或狀態無效");}
bool gain(float value){return std::isfinite(value) && std::abs(value)<=16777216.F;}
void valid_filter(const VoiceGraph::Filter& value){require(value.type<4 && std::isfinite(value.frequency) && value.frequency>=0 && value.frequency<=1 && std::isfinite(value.one_over_q) && value.one_over_q>0 && value.one_over_q<=1.5F);}
struct FilterState {
    VoiceGraph::Filter parameters;
    std::array<float,maximum_channels> low{},band{};
    float process(float sample,unsigned channel){
        // Chamberlin 狀態變數濾波器，與 XAudio2 frequency=2*sin(pi*f/rate) 的參數相符。
        low[channel]+=parameters.frequency*band[channel];
        const float high=sample-low[channel]-parameters.one_over_q*band[channel];
        band[channel]+=parameters.frequency*high;
        switch(parameters.type){case 0:return low[channel];case 1:return band[channel];case 2:return high;default:return high+low[channel];}
    }
    void reset(){low.fill(0);band.fill(0);}
};
unsigned default_mask(unsigned channels){
    constexpr unsigned masks[]{0,4,3,0xb,0x33,0x607,0x60f,0x70f,0x63f};
    return channels<9?masks[channels]:0;
}
std::vector<float> default_matrix(const VoiceGraph::Format& from,const VoiceGraph::Format& to){
    const auto n=from.channels,m=to.channels;std::vector<float> result(n*m);
    const auto sm=from.channel_mask?from.channel_mask:default_mask(n),dm=to.channel_mask?to.channel_mask:default_mask(m);
    // Mono 的雙前聲道特例只適用於未指定方位或 FrontCenter。
    if(n==1 && (!from.channel_mask || sm==4) && (dm&3)==3){result[0]=result[n]=1;return result;}
    const bool five_one=n==6 && m==6 && (sm==0x3f || sm==0x60f) && (dm==0x3f || dm==0x60f);
    if(n==m && (sm==dm || five_one)){for(unsigned i=0;i<n;++i)result[i*n+i]=1;return result;}
    if(n==2 && m==1 && sm==3 && dm==4){result[0]=result[1]=.5F;return result;}
    if(!sm || !dm){for(unsigned i=0;i<std::min(n,m);++i)result[i*n+i]=1;return result;}
    unsigned si=0;
    for(unsigned bit=0;bit<32;++bit){const auto speaker=1U<<bit;if(!(sm&speaker))continue;
        if(dm&speaker){const auto di=std::popcount(dm&(speaker-1));result[static_cast<unsigned>(di)*n+si]=1;}
        else if(m<=2){
            // LFE 不隱含混入一般揚聲器；中央及環繞以等功率係數混入左右。
            if(speaker!=8){const bool left=(speaker&(1U|16U|64U|512U))!=0,right=(speaker&(2U|32U|128U|1024U))!=0;
                if(m==1)result[si]=(left||right)?.35355339F:.70710678F;
                else {if(!right)result[si]=.70710678F;if(!left)result[n+si]=.70710678F;}}
        }
        ++si;
    }
    return result;
}
float sample(std::span<const std::uint8_t> data,std::size_t frame,unsigned channel,const VoiceGraph::Format& format){
    const auto p=data.data()+(frame*format.channels+channel)*(format.bits/8);
    if(format.floating)return detail::pcm_sample<32,false>(p);
    if(format.bits==8)return detail::pcm_sample<8>(p);
    if(format.bits==16)return detail::pcm_sample<16>(p);
    std::uint32_t raw=0;for(unsigned i=0;i<format.bits/8;++i)raw|=std::uint32_t{p[i]}<<(i*8);
    if(format.bits==24)raw<<=8;
    return static_cast<float>(std::bit_cast<std::int32_t>(raw))*(1.F/2147483648.F);
}
}
struct VoiceGraph::Impl {
    struct Block {Packet packet;double cursor{};unsigned loops{};bool started{},done{},exit_loop{};};
    struct Edge {Voice destination;std::vector<float> matrix;bool use_filter{};FilterState filter;};
    struct Node {
        State state;float maximum_ratio{2};bool tails{},reset_samples{};
        std::array<float,maximum_channels> frame{};FilterState filter;
        std::vector<Edge> sends;std::vector<std::shared_ptr<Effect>> effects;
        std::array<std::optional<Block>,maximum_packets> packets;
        std::array<unsigned,maximum_packets> queue{};unsigned head{},count{};
        double played{},advance_pending{};
    };
    mutable std::mutex mutex;
    std::map<Voice,std::unique_ptr<Node>> nodes;std::vector<std::pair<Voice,Node*>> order;
    std::map<std::uint32_t,std::vector<Change>> changes;
    std::array<Event,16384> events{};unsigned event_count{};
    Voice next{1},master{};bool started{true},faulted{};
    Node& node(Voice id){const auto i=nodes.find(id);require(i!=nodes.end());return *i->second;}
    const Node& node(Voice id)const {const auto i=nodes.find(id);require(i!=nodes.end());return *i->second;}
    Edge& edge(Node& n,Voice destination){if(!destination)destination=master;for(auto& e:n.sends)if(e.destination==destination)return e;throw std::invalid_argument("音訊輸出連線不存在");}
    void rebuild(){
        order.clear();for(auto& [id,n]:nodes)order.emplace_back(id,n.get());
        std::stable_sort(order.begin(),order.end(),[](const auto& a,const auto& b){
            if(a.second->state.kind!=b.second->state.kind)return a.second->state.kind<b.second->state.kind;
            return a.second->state.stage<b.second->state.stage;
        });
    }
    void event(EventKind kind,Voice voice,std::uint64_t context){
        if(event_count+1<events.size())events[event_count++]={kind,voice,context};
        else if(!faulted){faulted=true;events[event_count++]={EventKind::CriticalError,0,0};}
    }
    void validate(const Change& c){
        auto& n=node(c.voice);
        switch(c.operation){
        case Operation::Start:case Operation::Stop:case Operation::ExitLoop:require(n.state.kind==Kind::Source);break;
        case Operation::Volume:require(gain(c.value));break;
        case Operation::Frequency:require(n.state.kind==Kind::Source && std::isfinite(c.value) && !(n.state.flags&2U));break;
        case Operation::ChannelVolumes:require(c.values.size()==n.state.format.channels && std::all_of(c.values.begin(),c.values.end(),gain));break;
        case Operation::Filter:require(n.state.flags&8U);valid_filter(c.filter);break;
        case Operation::OutputFilter:require(edge(n,c.destination).use_filter);valid_filter(c.filter);break;
        case Operation::Matrix:require(c.values.size()==edge(n,c.destination).matrix.size() && std::all_of(c.values.begin(),c.values.end(),gain));break;
        }
    }
    void apply(const Change& c){
        auto& n=node(c.voice);
        switch(c.operation){
        case Operation::Start:n.state.playing=true;n.tails=false;break;
        case Operation::Stop:n.state.playing=false;n.tails=c.tails;if(!c.tails){n.filter.reset();for(auto& effect:n.effects)effect->reset();}break;
        case Operation::ExitLoop:if(n.count)n.packets[n.queue[n.head]]->exit_loop=true;break;
        case Operation::Volume:n.state.volume=c.value;break;
        case Operation::ChannelVolumes:std::copy(c.values.begin(),c.values.end(),n.state.channel_volumes.begin());break;
        case Operation::Frequency:n.state.ratio=std::clamp(c.value,(1.F/1024),n.maximum_ratio);break;
        case Operation::Filter:n.state.filter=n.filter.parameters=c.filter;break;
        case Operation::OutputFilter:edge(n,c.destination).filter.parameters=c.filter;break;
        case Operation::Matrix:std::copy(c.values.begin(),c.values.end(),edge(n,c.destination).matrix.begin());break;
        }
    }
    void finish(Voice id,Node& n,Block& b){
        event(EventKind::BufferEnd,id,b.packet.context);
        if(b.packet.end_stream){event(EventKind::StreamEnd,id,0);n.played=0;}
        b.done=true;n.head=(n.head+1)%maximum_packets;--n.count;
    }
    double advance(Voice id,Node& n,double amount,bool pause){
        // 每一段邊界均保留回呼順序；高倍率也不跳過短 buffer／loop。
        while(n.count && amount>0 && !faulted){
            auto& b=*n.packets[n.queue[n.head]];auto& p=b.packet;
            if(!b.started){b.started=true;event(EventKind::BufferStart,id,p.context);if(pause)return amount;}
            const auto end=p.play_begin+p.play_length,loop_end=p.loop_begin+p.loop_length;
            const bool loops=b.loops && !b.exit_loop && b.cursor<loop_end;
            const double boundary=loops?loop_end:end,step=std::min(amount,boundary-b.cursor);
            b.cursor+=step;n.played+=step;amount-=step;
            if(b.cursor<boundary)break;
            if(loops){if(b.loops!=255)--b.loops;b.cursor=p.loop_begin;event(EventKind::LoopEnd,id,p.context);}
            else finish(id,n,b);
            if(pause)return amount;
        }
        return n.count?amount:0;
    }
};
VoiceGraph::VoiceGraph():impl_(std::make_unique<Impl>()){}
VoiceGraph::~VoiceGraph()=default;
VoiceGraph::Voice VoiceGraph::create(Kind kind,Format format,unsigned flags,unsigned stage,float maximum_ratio){
    require(format.channels && format.channels<=maximum_channels && format.rate>=1000 && format.rate<=384000);
    require((format.floating?format.bits==32:format.bits==8 || format.bits==16 || format.bits==24 || format.bits==32) && (!format.channel_mask || static_cast<unsigned>(std::popcount(format.channel_mask))==format.channels));
    require(std::isfinite(maximum_ratio) && maximum_ratio>=(1.F/1024) && maximum_ratio<=1024);
    require(!(flags&~14U) && (kind==Kind::Source || !(flags&6U)));const auto creation_flags=flags;if(flags&4U)flags|=2U;
    std::lock_guard lock(impl_->mutex);if(kind==Kind::Master)require(!impl_->master && format.channels<=2);else require(impl_->master!=0);
    auto n=std::make_unique<Impl::Node>();n->state.kind=kind;n->state.format=format;n->state.flags=flags;n->state.creation_flags=creation_flags;n->state.stage=stage;n->state.channel_volumes.assign(format.channels,1);n->maximum_ratio=maximum_ratio;n->state.ratio=std::min(1.F,maximum_ratio);
    const auto id=impl_->next++;impl_->nodes.emplace(id,std::move(n));if(kind==Kind::Master)impl_->master=id;impl_->rebuild();return id;
}
void VoiceGraph::destroy(Voice voice){
    std::lock_guard lock(impl_->mutex);impl_->node(voice);if(voice==impl_->master)require(impl_->nodes.size()==1);
    for(const auto& [id,n]:impl_->nodes)if(id!=voice)for(const auto& e:n->sends)require(e.destination!=voice);
    // 尚未 commit 的操作隨音源銷毀失效，不能落到日後另一個音源。
    for(auto& [set,list]:impl_->changes)std::erase_if(list,[&](const auto& c){return c.voice==voice || c.destination==voice;});
    impl_->nodes.erase(voice);if(impl_->master==voice)impl_->master=0;impl_->rebuild();
}
void VoiceGraph::set_sends(Voice voice,std::span<const Send> sends){
    std::lock_guard lock(impl_->mutex);auto& n=impl_->node(voice);require(n.state.kind!=Kind::Master);std::vector<Impl::Edge> edges;
    unsigned output_rate=0;
    for(const auto& s:sends){const auto destination=s.destination?s.destination:impl_->master;const auto& d=impl_->node(destination);
        require(d.state.kind!=Kind::Source && (n.state.kind==Kind::Source || d.state.kind==Kind::Master || d.state.stage>n.state.stage));
        require(!output_rate || output_rate==d.state.format.rate);output_rate=d.state.format.rate;
        require(!(n.state.flags&4U) || output_rate==n.state.format.rate);
        require(std::none_of(edges.begin(),edges.end(),[&](const auto& e){return e.destination==destination;}));
        auto matrix=s.matrix.empty()?default_matrix(n.state.format,d.state.format):s.matrix;
        require(matrix.size()==n.state.format.channels*d.state.format.channels && std::all_of(matrix.begin(),matrix.end(),gain));
        edges.push_back({destination,std::move(matrix),s.use_filter,{}});
    }
    n.sends=std::move(edges);
}
void VoiceGraph::submit(Voice voice,Packet packet){
    std::lock_guard lock(impl_->mutex);auto& n=impl_->node(voice);require(n.state.kind==Kind::Source);
    const unsigned stride=n.state.format.channels*n.state.format.bits/8;require(!packet.pcm.empty() && packet.pcm.size()%stride==0 && packet.pcm.size()<=0x80000000ULL);
    const auto frames=packet.pcm.size()/stride;
    if(!packet.play_length){require(!packet.play_begin);packet.play_length=static_cast<unsigned>(frames);}
    require(packet.play_begin<frames && packet.play_length<=frames-packet.play_begin && packet.loop_count<=255);
    if(packet.loop_count){if(!packet.loop_length)packet.loop_length=static_cast<unsigned>(frames)-packet.loop_begin;
        require(packet.loop_begin<frames && packet.loop_length && packet.loop_length<=frames-packet.loop_begin && packet.loop_begin<packet.play_begin+packet.play_length && packet.loop_begin+packet.loop_length>packet.play_begin && packet.loop_begin+packet.loop_length<=packet.play_begin+packet.play_length);
    }else require(!packet.loop_begin && !packet.loop_length);
    require(n.count<maximum_packets);auto slot=std::find_if(n.packets.begin(),n.packets.end(),[](const auto& p){return !p || p->done;});require(slot!=n.packets.end());
    const unsigned index=static_cast<unsigned>(slot-n.packets.begin()),begin=packet.play_begin,loops=packet.loop_count;
    *slot=Impl::Block{std::move(packet),static_cast<double>(begin),loops};n.queue[(n.head+n.count)%maximum_packets]=index;++n.count;
    if(n.reset_samples){n.played=0;n.reset_samples=false;}
}
void VoiceGraph::flush(Voice voice){
    std::lock_guard lock(impl_->mutex);auto& n=impl_->node(voice);require(n.state.kind==Kind::Source);
    const unsigned keep=n.state.playing && n.count && n.packets[n.queue[n.head]]->started?1U:0U;
    for(unsigned i=keep;i<n.count;++i){auto& b=*n.packets[n.queue[(n.head+i)%maximum_packets]];b.done=true;impl_->event(EventKind::BufferEnd,voice,b.packet.context);}
    n.count=keep;if(!n.state.playing){n.reset_samples=true;n.advance_pending=0;}
}
void VoiceGraph::set_sample_rate(Voice voice,unsigned rate){std::lock_guard lock(impl_->mutex);auto& n=impl_->node(voice);require(n.state.kind==Kind::Source && !n.count && rate>=1000 && rate<=384000);
    if(n.state.flags&4U)for(const auto& e:n.sends)require(impl_->node(e.destination).state.format.rate==rate);
    n.state.format.rate=rate;
}
void VoiceGraph::discontinuity(Voice voice){std::lock_guard lock(impl_->mutex);auto& n=impl_->node(voice);require(n.state.kind==Kind::Source);if(n.count)n.packets[n.queue[(n.head+n.count-1)%maximum_packets]]->packet.end_stream=true;}
VoiceGraph::State VoiceGraph::state(Voice voice)const {
    std::lock_guard lock(impl_->mutex);const auto& n=impl_->node(voice);auto state=n.state;state.queued=n.count;state.samples=static_cast<std::uint64_t>(n.played);state.context=n.count?n.packets[n.queue[n.head]]->packet.context:0;return state;
}
std::uint32_t VoiceGraph::required_bytes(Voice voice,unsigned frames,unsigned rate)const {
    std::lock_guard lock(impl_->mutex);const auto& n=impl_->node(voice);require(n.state.kind==Kind::Source && rate);
    double needed=std::ceil(static_cast<double>(frames)*n.state.format.rate*n.state.ratio/rate);
    for(unsigned i=0;i<n.count && needed>0;++i){const auto& b=*n.packets[n.queue[(n.head+i)%maximum_packets]];const auto& p=b.packet;
        double available=p.play_begin+p.play_length-b.cursor;
        if(b.loops && !b.exit_loop){if(b.loops==255)return 0;available+=static_cast<double>(b.loops)*p.loop_length;}
        needed-=available;
    }
    if(needed<=0)return 0;return static_cast<std::uint32_t>(std::min(double(UINT32_MAX),std::ceil(needed)*n.state.format.channels*n.state.format.bits/8));
}
std::vector<float> VoiceGraph::matrix(Voice voice,Voice destination)const {std::lock_guard lock(impl_->mutex);return impl_->edge(impl_->node(voice),destination).matrix;}
VoiceGraph::Filter VoiceGraph::output_filter(Voice voice,Voice destination)const {std::lock_guard lock(impl_->mutex);return impl_->edge(impl_->node(voice),destination).filter.parameters;}
void VoiceGraph::change(Change change,std::uint32_t set){require(set!=UINT32_MAX);std::lock_guard lock(impl_->mutex);impl_->validate(change);if(set)impl_->changes[set].push_back(std::move(change));else impl_->apply(change);}
void VoiceGraph::commit(std::uint32_t set){
    std::lock_guard lock(impl_->mutex);
    // 連線可能在排程後改變；先檢查整組，錯誤時不部分套用。
    for(const auto& [id,list]:impl_->changes)if(!set || id==set)for(const auto& c:list)impl_->validate(c);
    for(auto i=impl_->changes.begin();i!=impl_->changes.end();)if(!set || i->first==set){for(const auto& c:i->second)impl_->apply(c);i=impl_->changes.erase(i);}else ++i;
}
void VoiceGraph::set_effects(Voice voice,std::vector<std::shared_ptr<Effect>> effects){std::lock_guard lock(impl_->mutex);auto& n=impl_->node(voice);require(n.state.format.channels<=2 && std::all_of(effects.begin(),effects.end(),[](const auto& p){return bool(p);}));n.effects=std::move(effects);}
void VoiceGraph::start(bool active){std::lock_guard lock(impl_->mutex);impl_->started=active;}
bool VoiceGraph::running()const {std::lock_guard lock(impl_->mutex);return impl_->started && !impl_->faulted;}
std::vector<VoiceGraph::Event> VoiceGraph::take_events(){
    std::lock_guard lock(impl_->mutex);std::vector<Event> events(impl_->events.begin(),impl_->events.begin()+impl_->event_count);impl_->event_count=0;
    for(auto& [id,n]:impl_->nodes)for(auto& b:n->packets)if(b && b->done)b.reset();return events;
}
std::size_t VoiceGraph::render_frames(std::span<float> output,unsigned rate,bool pause) {
    std::fill(output.begin(),output.end(),0);if(!rate)return 0;std::lock_guard lock(impl_->mutex);
    if(!impl_->started || impl_->faulted || !impl_->master)return output.size()/2;
    auto& master=*impl_->nodes.at(impl_->master);
    for(std::size_t frame=0;frame+1<output.size() && !impl_->faulted;frame+=2){
        for(auto& [id,n]:impl_->order)if(n->state.kind==Kind::Source && n->state.playing){
            if(n->advance_pending>0)n->advance_pending=impl_->advance(id,*n,n->advance_pending,pause);
            if(pause && impl_->event_count)return frame/2;
            if(n->count){auto& b=*n->packets[n->queue[n->head]];if(!b.started){b.started=true;impl_->event(EventKind::BufferStart,id,b.packet.context);if(pause)return frame/2;}}
        }
        for(auto& [id,n]:impl_->order)n->frame.fill(0);
        for(auto& [id,n]:impl_->order){const auto channels=n->state.format.channels;
            if(n->state.kind==Kind::Source){
                if(!n->state.playing && !n->tails)continue;
                if(n->state.playing && n->count){
                    auto& b=*n->packets[n->queue[n->head]];const auto& p=b.packet;
                    if(!b.started){b.started=true;impl_->event(EventKind::BufferStart,id,p.context);}
                    const auto at=static_cast<std::size_t>(b.cursor);const auto end=p.play_begin+p.play_length;
                    auto next=std::min(at+1,static_cast<std::size_t>(end-1));
                    if(b.loops && !b.exit_loop && next>=p.loop_begin+p.loop_length)next=p.loop_begin;
                    const auto fraction=static_cast<float>(b.cursor-static_cast<double>(at));
                    for(unsigned c=0;c<channels;++c){const auto first=sample(p.pcm,at,c,n->state.format),last=sample(p.pcm,next,c,n->state.format);n->frame[c]=first+(last-first)*fraction;}
                    n->advance_pending=impl_->advance(id,*n,static_cast<double>(n->state.format.rate)*n->state.ratio/rate,pause);
                }
            }
            if(n->state.flags&8U)for(unsigned c=0;c<channels;++c)n->frame[c]=n->filter.process(n->frame[c],c);
            for(unsigned c=0;c<channels;++c)n->frame[c]*=n->state.volume;
            for(auto& effect:n->effects){float right=channels==1?n->frame[0]:n->frame[1];effect->process(n->frame[0],right);if(channels>1)n->frame[1]=right;}
            for(unsigned c=0;c<channels;++c)n->frame[c]*=n->state.channel_volumes[c];
            for(auto& e:n->sends){auto& destination=*impl_->nodes.at(e.destination);
                for(unsigned d=0;d<destination.state.format.channels;++d){float sum=0;for(unsigned c=0;c<channels;++c)sum+=n->frame[c]*e.matrix[d*channels+c];if(e.use_filter)sum=e.filter.process(sum,d);destination.frame[d]+=sum;}
            }
        }
        output[frame]=master.frame[0];output[frame+1]=master.frame[master.state.format.channels==1?0:1];
        if(pause && impl_->event_count)return frame/2+1;
    }
    return output.size()/2;
}
void VoiceGraph::render(std::span<float> output,unsigned rate) noexcept {(void)render_frames(output,rate,false);}
std::size_t VoiceGraph::render_until_event(std::span<float> output,unsigned rate){return render_frames(output,rate,true);}
}
