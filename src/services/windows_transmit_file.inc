// 檔案與前後緩衝先形成當次傳輸內容，送出、排序、取消及完成通知沿用 socket I/O。
io.transmit_file=true;io.transmit_flags=d(6);io.send_chunk=d(3);
sockaddr_storage peer{};socklen_t peer_size=sizeof peer;
const auto file=files_.find(arg(1));
if(socket.type!=SOCK_STREAM || (d(6)&~0x37U) || ((d(6)&2U) && !(d(6)&1U)) ||
   ((d(6)&4U) && (d(6)&3U)) || (d(6)&0x30U)==0x30U || d(2)>0x7ffffffeU)error=10022;
else if(socket.send_shutdown)error=10058;
else if(::getpeername(socket.descriptor,reinterpret_cast<sockaddr*>(&peer),&peer_size))error=10057;
else if(arg(1) && (file==files_.end() || (!file->second.native && !file->second.bytes)))error=6;
else if(arg(1) && !(file->second.access&1U))error=5;
else{
    std::uint64_t head=0,tail=0,head_size=0,tail_size=0;
    if(arg(5)){
        memory.check(GuestAddress{arg(5)},x86_?16:32,Access::Read);
        head=memory.load(GuestAddress{arg(5)},bits);head_size=memory.load(GuestAddress{arg(5)+unit},32);
        tail=memory.load(GuestAddress{arg(5)+2*unit},bits);tail_size=memory.load(GuestAddress{arg(5)+3*unit},32);
    }
    const auto load=[&](std::uint64_t offset,std::uint64_t size){
        if(offset>size || (d(2) && d(2)>size-offset)){error=10022;return;}
        const auto count=d(2)?std::uint64_t{d(2)}:size-offset;
        const auto total=head_size+count+tail_size;
        if(total>0x7ffffffeU){error=10022;return;}
        io.data.resize(static_cast<std::size_t>(total));
        if(head_size)memory.read(GuestAddress{head},std::span(io.data).first(static_cast<std::size_t>(head_size)));
        if(tail_size)memory.read(GuestAddress{tail},std::span(io.data).last(static_cast<std::size_t>(tail_size)));
        if(!count)return;
        auto& source=file->second;auto destination=std::span(io.data).subspan(static_cast<std::size_t>(head_size),static_cast<std::size_t>(count));
        if(source.native){
            std::size_t copied=0;
            while(copied<destination.size()){
                const auto read=source.native->read(destination.subspan(copied,std::min<std::size_t>(destination.size()-copied,1024*1024)),offset+copied);
                if(read.error){error=read.error;return;}if(!read.bytes){error=38;return;}
                copied+=static_cast<std::size_t>(read.bytes);
            }
        }else {std::copy_n(source.bytes->begin()+static_cast<std::ptrdiff_t>(offset),destination.size(),destination.begin());process_io::record(process_io::Kind::Read,count);}
        if(!io.overlapped)runtime_.win_file_positions.at(source.object)=offset+count;
    };
    if(!arg(1)){if(d(2))error=10022;else load(0,0);}
    else{
        const auto& source=file->second;const auto position_guard=runtime_.win_file_positions.at(source.object).lock();
        const auto offset=io.overlapped?memory.load(GuestAddress{io.overlapped+2*unit},64):static_cast<std::uint64_t>(runtime_.win_file_positions.at(source.object));
        load(offset,source.native?source.native->current_size():source.bytes->size());
    }
}
