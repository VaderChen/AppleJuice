// 位於 MF 共用狀態命名空間內：媒體工作階段的宿主管線狀態，不含客體 ABI；回呼由 mf_session_helpers.inc 排入客體工作執行緒。
struct SessionVideoFrame {std::int64_t time{};std::uint64_t duration{};std::vector<std::uint8_t> bytes;};
struct SessionVideoDecoder {video::DecoderConfig config;std::optional<video::HardwareDecoder> decoder;bool flush{};};
struct SessionAudioChunk {std::int64_t time{};std::uint64_t duration{};std::vector<std::uint8_t> bytes;};
// 解碼與色彩轉換都在媒體工作執行緒完成，不佔用客體服務邊界；同一解碼器同時只有一個工作。
inline std::vector<SessionVideoFrame> session_decode_job(std::shared_ptr<SessionVideoDecoder> holder,std::vector<std::uint8_t> packet,std::int64_t time,std::uint64_t duration,mf::VideoLayout layout,std::int64_t target,bool drain){
    if(drain && (!holder->decoder || holder->flush))return {};
    if(!holder->decoder)holder->decoder.emplace(holder->config);else if(holder->flush)holder->decoder->flush();holder->flush=false;
    auto frames=drain?holder->decoder->drain():holder->decoder->decode(packet,video::Timestamp{time,10000000},video::Timestamp{std::bit_cast<std::int64_t>(duration),10000000});
    const auto ticks=[](const video::Timestamp& stamp){return stamp.timescale?static_cast<std::int64_t>(static_cast<__int128>(stamp.value)*10000000/stamp.timescale):0;};
    std::vector<SessionVideoFrame> result;result.reserve(frames.size());
    for(auto& frame:frames){
        SessionVideoFrame out;out.time=ticks(frame.pts);const auto decoded_duration=ticks(frame.duration);out.duration=decoded_duration>0?static_cast<std::uint64_t>(decoded_duration):duration;
        if(out.time>=target)out.bytes=mf::convert_video_frame(frame,layout);
        result.push_back(std::move(out));
    }
    return result;
}
struct SessionMedia {
    static constexpr std::size_t max_frames=8,reorder_limit=6,max_chunks=4;
    static constexpr std::int64_t renderer_lead=2000000; // 宿主音訊裝置先取得 200 ms，避免首次 underrun。
    std::uint64_t source{},descriptor{},activate{},callback{},attributes{};
    std::shared_ptr<FileSourceState> file;
    const mf::Mp4Track* track{};
    bool video{},renderer{},ignore_clock{},has_ex{},qi_pending{},shutdown_on_remove{},discontinuity{true};
    std::int64_t time_offset{},target{},last_end{};
    std::uint64_t guest_block{},guest_buffer{};std::size_t guest_capacity{};
    // 取樣與客體 byte stream 讀取。讀取可能跨越服務續行，先記錄請求再於 pump 開頭完成。
    std::uint64_t next_packet{},packet_at{};std::size_t packet_size{};std::vector<std::uint8_t> packet;bool read_pending{},packet_ready{};
    bool demux_done{},drain_submitted{},drained{};
    // 視訊
    std::shared_ptr<SessionVideoDecoder> decoder;unsigned nal_length{4};mf::VideoLayout layout;
    std::future<std::vector<SessionVideoFrame>> decoding;std::map<std::int64_t,SessionVideoFrame> frames;std::size_t presented{};
    // 音訊
    std::shared_ptr<mf::AudioTranscoder> transcoder;mf::AudioOutput output;std::int64_t base_time{};std::uint64_t out_frames{};std::deque<SessionAudioChunk> chunks;
    std::shared_ptr<audio::Buffer> device;std::unique_ptr<audio::Output> device_output;
    // 下一幀依顯示順序才能交付；解碼器遺失畫面時由重排序上限或 drain 放行，不會卡住。
    const SessionVideoFrame* next_frame()const{
        if(frames.empty())return nullptr;const auto& frame=frames.begin()->second;
        if(track && presented<track->presentation.size() && frame.time==track->presentation[presented])return &frame;
        return frames.size()>=reorder_limit || drained?&frame:nullptr;
    }
    bool frame_due(const SessionVideoFrame& frame,std::int64_t clock,bool scrub)const{return ignore_clock || scrub || frame.time-time_offset<=clock;}
    bool chunk_due(const SessionAudioChunk& chunk,std::int64_t clock)const{return ignore_clock || chunk.time-time_offset-(renderer?renderer_lead:0)<=clock;}
    bool ready(std::int64_t clock,bool scrub)const{
        if(video){
            if(decoding.valid()){if(decoding.wait_for(std::chrono::seconds(0))==std::future_status::ready)return true;}
            else if(!drain_submitted && frames.size()<max_frames)return true;
            const auto* frame=next_frame();return frame && frame_due(*frame,clock,scrub);
        }
        if(!demux_done && chunks.size()<max_chunks)return true;
        return !chunks.empty() && chunk_due(chunks.front(),clock);
    }
    bool source_done()const{return video?drained:demux_done;}
    bool finished(std::int64_t clock)const{return source_done() && frames.empty() && chunks.empty() && !decoding.valid() && (ignore_clock || clock>=last_end);}
};
struct SessionCommand {enum Kind {SetTopology,ClearTopologies,Start,Pause,Stop,Close,SetRate} kind{};std::uint32_t flags{};std::uint64_t topology{};Value position;float rate{1};};
struct SessionState {
    std::deque<SessionCommand> commands;std::vector<std::shared_ptr<SessionMedia>> media;
    std::uint64_t topology{};std::deque<std::uint64_t> queued;
    // MFCLOCK_STATE：0 尚未啟動、1 執行、2 停止、3 暫停。
    std::uint32_t clock_state{},continuity{};std::uint64_t anchor_system{};std::int64_t anchor_time{};float rate{1};
    std::vector<std::uint64_t> clock_sinks;std::uint64_t time_source{};
    bool scrub{},eop_sent{},closed_session{},torn_down{},failed{};
    float master_volume{1};bool muted{};std::array<float,2> channel_volume{1,1};
    std::int64_t clock_time(std::uint64_t now)const{
        if(clock_state!=1)return anchor_time;const auto elapsed=now>anchor_system?now-anchor_system:0;
        return anchor_time+(rate==1?static_cast<std::int64_t>(elapsed):static_cast<std::int64_t>(static_cast<double>(elapsed)*rate));
    }
    bool quiescent()const{return std::none_of(media.begin(),media.end(),[](const auto& m){return m->decoding.valid() && m->decoding.wait_for(std::chrono::seconds(0))!=std::future_status::ready;});}
    bool command_resets_pipeline(const SessionCommand& command)const{
        const bool active=clock_state==1 || clock_state==3;
        switch(command.kind){
        case SessionCommand::ClearTopologies:case SessionCommand::SetRate:case SessionCommand::Pause:return false;
        case SessionCommand::Start:return command.position.type!=0 || !active;
        case SessionCommand::SetTopology:return (command.flags&5U)!=0 || !topology || !active;
        default:return true;
        }
    }
    bool pump_ready(std::uint64_t now)const{
        for(const auto& m:media)if(m->read_pending || m->qi_pending)return true;
        if(!commands.empty())return !command_resets_pipeline(commands.front()) || quiescent();
        if(clock_state!=1 || media.empty())return false;
        const auto clock=clock_time(now);bool all=true;
        for(const auto& m:media){if(m->ready(clock,scrub))return true;all=all && m->finished(clock);}
        return all;
    }
};
