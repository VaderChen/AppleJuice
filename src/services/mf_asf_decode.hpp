// 位於 MF 共用狀態命名空間內：ASF Source Reader 的宿主解碼狀態，不含客體 ABI。
// 每條串流一份輸出佇列；要求解碼的串流另有 VC-1／WMV 或 WMA 解碼器（x64arm/media_decode.hpp），其餘串流原樣交付壓縮樣本。
// 解多工與解碼都在宿主完成，客體樣本由呼叫端在 API 邊界從佇列建立。
struct AsfDecodedSample {std::vector<std::uint8_t> bytes;std::int64_t time{};std::optional<std::uint64_t> duration;bool key{},discontinuity{};};
struct AsfStreamOutput {
    // 視訊：解碼後依 layout 轉為要求的未壓縮格式。
    std::unique_ptr<media::VideoDecoder> video;mf::VideoLayout layout;std::uint64_t frame_time{};
    // 輸入樣本時間由小到大配給輸出畫面：ASF 的時間不論依解碼或顯示順序記錄，輸出都維持單調遞增。
    std::multiset<std::int64_t> pending_times;
    // 音訊：解碼為交錯浮點，必要時經 AudioToolbox 轉為要求的 PCM／浮點格式；時間依樣本數連續累計。
    std::unique_ptr<media::AudioDecoder> audio;std::unique_ptr<mf::AudioTranscoder> transcoder;mf::AudioOutput output;std::int64_t audio_next{INT64_MIN};
    std::deque<AsfDecodedSample> ready;bool ended{},discontinuity{};
    static constexpr std::size_t reorder_limit=32;
    bool decoding()const{return video || audio;}
    void push(std::vector<std::uint8_t> bytes,std::int64_t time,std::optional<std::uint64_t> duration,bool key){
        ready.push_back({std::move(bytes),time,duration,key,std::exchange(discontinuity,false)});
    }
    void emit_video(std::vector<video::DecodedFrame> frames,std::int64_t fallback){
        for(auto& frame:frames){
            std::int64_t time=fallback;
            if(!pending_times.empty()){time=*pending_times.begin();pending_times.erase(pending_times.begin());}
            else if(frame.pts.timescale)time=static_cast<std::int64_t>(static_cast<__int128>(frame.pts.value)*10000000/frame.pts.timescale);
            std::optional<std::uint64_t> duration;
            if(frame_time)duration=frame_time;else if(frame.duration.timescale && frame.duration.value>0)duration=static_cast<std::uint64_t>(static_cast<__int128>(frame.duration.value)*10000000/frame.duration.timescale);
            push(mf::convert_video_frame(frame,layout),time,duration,true);
        }
    }
    void emit_audio(std::vector<media::DecodedAudio> blocks,std::int64_t fallback){
        for(auto& block:blocks){
            if(!block.frames() || !block.rate)continue;
            if(audio_next==INT64_MIN)audio_next=block.time!=INT64_MIN?block.time:fallback;
            const auto time=audio_next;audio_next+=static_cast<std::int64_t>(static_cast<__int128>(block.frames())*10000000/block.rate);
            std::vector<std::uint8_t> bytes(block.samples.size()*sizeof(float));std::memcpy(bytes.data(),block.samples.data(),bytes.size());
            if(transcoder)bytes=transcoder->push(bytes);
            if(bytes.empty())continue;
            const auto frames=bytes.size()/output.frame_bytes();
            push(std::move(bytes),time,static_cast<std::uint64_t>(static_cast<__int128>(frames)*10000000/output.rate),true);
        }
    }
    // 一個已解多工的媒體物件：解碼串流送入解碼器，其他串流原樣排入。
    void consume(const mf::AsfObject& object){
        if(video){
            pending_times.insert(object.time);
            emit_video(video->decode(object.bytes,object.time,object.key),object.time);
            // 解碼器丟棄損毀畫面時，對應的時間不會被取走；超過重排序深度即捨棄最舊的時間，避免後續畫面逐步提前。
            while(pending_times.size()>reorder_limit)pending_times.erase(pending_times.begin());
        }else if(audio)emit_audio(audio->decode(object.bytes,object.time),object.time);
        else push(object.bytes,object.time,frame_time?std::optional<std::uint64_t>{frame_time}:std::nullopt,object.key);
    }
    // 串流結束：交出解碼器延遲中的資料。
    void finish(){
        if(ended)return;
        if(video)emit_video(video->drain(),pending_times.empty()?0:*pending_times.begin());
        if(audio){
            emit_audio(audio->drain(),audio_next==INT64_MIN?0:audio_next);
            if(transcoder){
                auto bytes=transcoder->drain();
                if(!bytes.empty()){const auto frames=bytes.size()/output.frame_bytes();push(std::move(bytes),audio_next==INT64_MIN?0:audio_next,static_cast<std::uint64_t>(static_cast<__int128>(frames)*10000000/output.rate),true);}
            }
        }
        ended=true;
    }
    // 清除（Flush）或跳轉：丟棄佇列與解碼器內的資料，下一個樣本標記不連續。
    void reset(bool seek){
        ready.clear();pending_times.clear();discontinuity=true;
        if(video)video->flush();
        if(audio){audio->flush();audio_next=INT64_MIN;if(transcoder)transcoder->reset();}
        if(seek)ended=false;
    }
};
struct AsfReaderDecode {
    std::vector<std::unique_ptr<AsfStreamOutput>> streams; // presentation 內的串流索引。
    bool pump_pending{}; // 讀取解碼工作正在客體 byte stream 回呼中；API 重新進入時必須先取回同一工作的結果。
    bool flush_requested{}; // 解碼讀取進行中收到 Flush：工作結束後清除佇列與解碼器。
    // 指定串流有輸出或已結束；任一串流（UINT32_MAX）時，任一已選取串流有輸出，或全部已選取串流都結束。
    bool satisfied(std::uint32_t index,const std::vector<bool>& selected)const{
        if(index!=UINT32_MAX)return !streams.at(index)->ready.empty() || streams.at(index)->ended;
        bool ended=true;
        for(std::size_t n=0;n<streams.size();++n)if(n<selected.size() && selected[n]){if(!streams[n]->ready.empty())return true;ended=ended && streams[n]->ended;}
        return ended;
    }
    // 取出下一個樣本；任一串流時取時間最早者。沒有樣本（串流結束）時回傳 nullopt，actual 為結束的串流索引。
    std::optional<AsfDecodedSample> take(std::uint32_t index,const std::vector<bool>& selected,std::uint32_t& actual){
        if(index==UINT32_MAX){
            std::optional<std::size_t> best;
            for(std::size_t n=0;n<streams.size();++n)if(n<selected.size() && selected[n] && !streams[n]->ready.empty() && (!best || streams[n]->ready.front().time<streams[*best]->ready.front().time))best=n;
            if(!best){actual=0;return std::nullopt;}
            index=static_cast<std::uint32_t>(*best);
        }
        actual=index;auto& queue=streams.at(index)->ready;if(queue.empty())return std::nullopt;
        auto sample=std::move(queue.front());queue.pop_front();return sample;
    }
};
