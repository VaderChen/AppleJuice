// WIN32_STREAM_ID 是 20 位元組的線上標頭，不使用宿主 struct 對齊；資料分段讀寫，不將大檔整份載入。
std::optional<std::uint32_t> WindowsServices::invoke_file_backup(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const bool seek=service.symbol=="BackupSeek",writing=service.symbol=="BackupWrite";
    const auto bits=static_cast<std::uint8_t>(x86_?32:64);const auto context=arg(seek?5:6);
    const auto finish=[&](std::uint32_t error){if(error)last_error_=error;arg.return_to(state,error?0:1);return std::optional<std::uint32_t>{};};
    const auto load=[](const std::vector<std::uint8_t>& bytes,std::size_t at,unsigned n){std::uint64_t value=0;for(unsigned i=0;i<n;++i)value|=std::uint64_t{bytes.at(at+i)}<<(8*i);return value;};
    try{
        if(!context)return finish(87);memory.check(GuestAddress{context},bits/8,Access::Write);auto id=memory.load(GuestAddress{context},bits);
        if(!seek && static_cast<std::uint32_t>(arg(4))){if(id && !file_backups_.erase(id))return finish(87);memory.store(GuestAddress{context},bits,0);return finish(0);}
        if(!arg(3) || (seek && !arg(4)))return finish(87);memory.store(GuestAddress{arg(3)},32,0);if(seek)memory.store(GuestAddress{arg(4)},32,0);
        const auto file=files_.find(arg(0));if(file==files_.end() || file->second.pipe || file->second.serial || file->second.hid || file->second.usb)return finish(6);
        auto& handle=file->second;const auto native=handle.native?handle.native:handle.identity;
        if(!native)return finish(6);
        if(!seek && !(handle.access&(writing?0x40000002U:0x80000001U)))return finish(5);
        if(!id){
            if(seek)return finish(87);FileBackup backup;backup.object=handle.object;backup.writing=writing;backup.security=static_cast<std::uint32_t>(arg(5))!=0;
            if(!writing){
                const auto info=native->information();if(info.error)return finish(info.error);
                const auto append=[&](unsigned type,unsigned attributes,std::uint64_t size,std::vector<std::uint8_t> data){
                    BackupStream stream;stream.type=type;stream.attributes=attributes;stream.size=size;stream.data=std::move(data);stream.header.resize(20);
                    const auto put=[&](unsigned at,std::uint64_t v,unsigned n){for(unsigned i=0;i<n;++i)stream.header[at+i]=static_cast<std::uint8_t>(v>>(8*i));};
                    put(0,type,4);put(4,attributes,4);put(8,size,8);backup.streams.push_back(std::move(stream));};
                if(backup.security){
                    if(!(handle.access&0x20000U))return finish(5);
                    const auto mask=7U|((handle.access&0x1000000U)?8U:0U);
                    auto descriptor=security::relative_descriptor(security::file_descriptor(security::read_native_file_security(*native,true),mask));
                    const auto size=descriptor.size();append(3,2,size,std::move(descriptor));
                }
                if(!(info.metadata.attributes&16))append(1,0,info.metadata.size,{});
            }
            id=runtime_.allocate_wait_handle();file_backups_.emplace(id,std::move(backup));memory.store(GuestAddress{context},bits,id);
        }
        const auto found=file_backups_.find(id);if(found==file_backups_.end() || found->second.object!=handle.object || (!seek && found->second.writing!=writing))return finish(87);
        auto& backup=found->second;
        const auto finish_write=[&]()->std::uint32_t{
            if(backup.type==1){if(const auto error=native->truncate(backup.size))return error;}
            else if(backup.type==3 && backup.security){
                auto descriptor=security::parse_relative_descriptor(backup.data);unsigned mask=0;
                if(!descriptor.data[0].empty())mask|=1;if(!descriptor.data[1].empty())mask|=2;if((descriptor.control&4))mask|=4;if((descriptor.control&16))mask|=8;
                if((mask&3) && !(handle.access&0x80000U))return 5;if((mask&4) && !(handle.access&0x40000U))return 5;if((mask&8) && !(handle.access&0x1000000U))return 5;
                security::write_virtual_file_security(*native,descriptor,mask);
            }
            backup.header.clear();backup.data.clear();backup.header_complete=false;backup.position=0;backup.size=0;return 0;};
        if(seek){
            const auto requested=static_cast<std::uint64_t>(static_cast<std::uint32_t>(arg(1)))|(static_cast<std::uint64_t>(static_cast<std::uint32_t>(arg(2)))<<32);std::uint64_t remaining=0;
            if(backup.writing){if(!backup.header_complete)return finish(25);remaining=backup.size-backup.position;}
            else {if(backup.stream>=backup.streams.size())return finish(25);const auto& stream=backup.streams[backup.stream];if(backup.position<stream.header.size())return finish(25);remaining=stream.size-(backup.position-stream.header.size());}
            const auto count=std::min(requested,remaining);backup.position+=count;
            memory.store(GuestAddress{arg(3)},32,count);memory.store(GuestAddress{arg(4)},32,count>>32);
            if(backup.writing && count==remaining){if(backup.type==3 && backup.security)backup.security=false;const auto error=finish_write();if(error)return finish(error);}
            else if(!backup.writing && count==remaining){++backup.stream;backup.position=0;}
            return finish(count==requested?0:25);
        }
        const auto length=static_cast<std::uint32_t>(arg(2));if(!length)return finish(0);if(!arg(1))return finish(87);
        memory.check(GuestAddress{arg(1)},length,writing?Access::Read:Access::Write);std::uint32_t done=0;
        while(done<length){
            if(!writing){
                if(backup.stream>=backup.streams.size())break;const auto& stream=backup.streams[backup.stream];
                if(backup.position<stream.header.size()){
                    const auto count=std::min<std::size_t>(length-done,stream.header.size()-backup.position);
                    memory.write(GuestAddress{arg(1)+done},std::span(stream.header).subspan(backup.position,count));backup.position+=count;done+=count;
                }else {
                    const auto at=backup.position-stream.header.size();const auto count=std::min<std::uint64_t>({length-done,stream.size-at,65536});
                    if(count){std::vector<std::uint8_t> data(count);if(stream.type==1){const auto read=native->read(data,at);if(read.error)return finish(read.error);if(read.bytes!=count)return finish(38);}else std::copy_n(stream.data.data()+at,count,data.begin());
                        memory.write(GuestAddress{arg(1)+done},data);backup.position+=count;done+=count;}
                    if(at+count==stream.size){++backup.stream;backup.position=0;}
                }
            }else if(!backup.header_complete){
                const auto target=backup.header.size()<20?20U:20U+backup.name_size;const auto count=std::min<std::size_t>(length-done,target-backup.header.size());
                const auto previous=backup.header.size();backup.header.resize(previous+count);memory.read(GuestAddress{arg(1)+done},std::span(backup.header).subspan(previous,count));done+=count;
                if(backup.header.size()>=20){
                    backup.type=static_cast<std::uint32_t>(load(backup.header,0,4));backup.attributes=static_cast<std::uint32_t>(load(backup.header,4,4));backup.size=load(backup.header,8,8);backup.name_size=static_cast<std::uint32_t>(load(backup.header,16,4));
                    if(backup.name_size>65534 || (backup.name_size&1) || backup.size>INT64_MAX)return finish(13);
                    if(backup.header.size()==20+backup.name_size){
                        if((backup.type!=1 && backup.type!=3) || backup.name_size)return finish(50); // 本檔案系統的串流列舉只提供預設資料與安全描述元。
                        if(backup.type==3 && backup.size>131072)return finish(13);
                        backup.header_complete=true;backup.position=0;if(backup.type==3 && backup.security)backup.data.resize(backup.size);
                        if(!backup.size){const auto error=finish_write();if(error)return finish(error);}
                    }
                }
            }else {
                const auto count=std::min<std::uint64_t>({length-done,backup.size-backup.position,65536});std::vector<std::uint8_t> bytes(count);memory.read(GuestAddress{arg(1)+done},bytes);
                if(backup.type==1){const auto written=native->write(bytes,backup.position);if(written.error || written.bytes!=count)return finish(written.error?written.error:112);}
                else if(backup.security)std::copy(bytes.begin(),bytes.end(),backup.data.data()+backup.position);
                backup.position+=count;done+=count;if(backup.position==backup.size){const auto error=finish_write();if(error)return finish(error);}
            }
            memory.store(GuestAddress{arg(3)},32,done);
        }
        return finish(0);
    }catch(const MemoryFault&){return finish(998);}catch(const SecurityDescriptorError& error){return finish(error.code);}catch(const std::bad_alloc&){return finish(8);}
}
