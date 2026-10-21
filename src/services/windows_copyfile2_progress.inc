const auto message=[&](unsigned type,unsigned error=0) -> std::optional<std::uint32_t>{
    if(!operation.callback)return std::nullopt;
    const unsigned unit=x86_?4U:8U,size=x86_?72U:80U;
    if(!operation.message){operation.message=allocate_heap(memory,size);if(!operation.message)return finish(8,false);}
    const auto address=operation.message;for(unsigned i=0;i<size;++i)memory.store(GuestAddress{address+i},8,0);
    const auto put=[&](unsigned at,unsigned bits,std::uint64_t value){memory.store(GuestAddress{address+at},static_cast<std::uint8_t>(bits),value);};
    put(0,32,type);
    if(type==6){
        put(8,32,operation.stage==6?3U:4U);put(12,32,1);put(16,32,copy_hresult(error));
        put(24,64,operation.chunk);put(32,64,operation.metadata.size);put(40,64,operation.position);put(48,64,operation.metadata.size);put(56,64,operation.position);
    }else{
        put(8,32,1);put(16,unit*8,operation.source_handle);put(16+unit,unit*8,operation.target_handle);
        const auto first=16+2*unit;
        if(type<=2){put(first,64,operation.chunk);put(first+8,64,operation.chunk_size);put(first+16,64,operation.metadata.size);
            if(type==1)put(first+24,64,operation.metadata.size);
            else{put(first+24,64,operation.position);put(first+32,64,operation.metadata.size);put(first+40,64,operation.position);}
        }else{put(first,64,operation.metadata.size);if(type==3)put(first+8,64,operation.metadata.size);
            else{put(first+8,64,operation.position);put(first+16,64,operation.metadata.size);put(first+24,64,operation.position);}}
    }
    operation.message_type=type;operation.pending=true;
    start_callbacks(state,memory,{{operation.callback,{address,operation.parameter}}},Continuation::FileTransfer,token);return std::nullopt;
};
const auto fail_copy=[&](unsigned error) -> std::optional<std::uint32_t>{
    if(operation.callback){operation.finish_error=error;operation.finishing=true;return message(6,error);}
    return finish(error,false);
};
if(operation.pending){
    operation.pending=false;
    if(operation.finishing)return finish(operation.finish_error,false);
    if(callback_result==1)return finish(1235,true);
    if(callback_result==2){
        if(operation.flags&2U)if(const auto error=save_restart(*operation.output,operation.metadata,operation.position,operation.resume_key))return finish(error,false);
        return finish(1235,false);
    }
    if(callback_result==3)operation.callback=0;
    if(callback_result==4 && operation.position<operation.metadata.size){
        if(operation.flags&2U)return finish(87,false);
        if(const auto error=save_restart(*operation.output,operation.metadata,operation.position,operation.resume_key))return finish(error,false);
        operation.finish_error=3050;operation.finishing=true;
        if(operation.message_type!=4 && operation.callback)return message(4);
        return finish(3050,false);
    }
    if(callback_result>4)return finish(87,false);
}
for(;;){
    if(operation.cancel && memory.load(GuestAddress{operation.cancel},32))return finish(1235,true);
    if(operation.stage==0){operation.stage=1;if(operation.callback)return message(3);}
    if(operation.stage==1){
        if(operation.position==operation.metadata.size){operation.stage=4;continue;}
        operation.chunk_size=std::min<std::uint64_t>(65536,operation.metadata.size-operation.position);operation.stage=2;
        if(operation.callback)return message(1);
    }
    if(operation.stage==2){
        std::array<std::uint8_t,65536> bytes{};const auto count=static_cast<std::size_t>(operation.chunk_size);
        std::uint64_t obtained=0;
        while(obtained<count){const auto read=operation.input->read(std::span(bytes).subspan(static_cast<std::size_t>(obtained),count-static_cast<std::size_t>(obtained)),operation.position+obtained);
            if(read.error || !read.bytes){operation.stage=6;return fail_copy(read.error?read.error:38U);}obtained+=read.bytes;}
        std::uint64_t written=0;
        while(written<obtained){const auto write=operation.output->write(std::span(bytes).subspan(static_cast<std::size_t>(written),static_cast<std::size_t>(obtained-written)),operation.position+written);
            if(write.error || !write.bytes){operation.stage=7;return fail_copy(write.error?write.error:112U);}written+=write.bytes;}
        operation.position+=obtained;runtime_.win_file_positions[operation.source_handle]=operation.position;runtime_.win_file_positions[operation.target_handle]=operation.position;
        operation.stage=3;if(operation.callback)return message(2);
    }
    if(operation.stage==3){++operation.chunk;operation.stage=1;continue;}
    if(operation.stage==4){
        if(const auto error=operation.output->truncate(operation.metadata.size))return fail_copy(error);
        if(const auto error=operation.output->set_times(operation.metadata.created,operation.metadata.accessed,operation.metadata.modified))return fail_copy(error);
        if(operation.flush)if(const auto error=operation.output->flush())return fail_copy(error);
        if(const auto error=filesystem_.set_attributes(operation.destination,operation.metadata.attributes&0x27U))return fail_copy(error);
        operation.stage=5;if(operation.callback)return message(4);
    }
    return finish(0,false);
}
