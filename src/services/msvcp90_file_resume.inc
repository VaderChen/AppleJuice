if(op.kind.starts_with("file_")){
    const auto file=ptr(op.buffer+l.file_pointer());
    const auto crt=[&](std::string_view name,std::vector<std::uint64_t> args,unsigned next,std::uint32_t wide=0,bool result64=false){
        return callback(resolve("crt",name).value,std::move(args),next,false,wide,result64);
    };
    if(op.kind=="file_construct"){
        if(!op.phase++){return callback(resolve("kernel32.dll","InitializeCriticalSection").value,{ptr(op.buffer+l.p)},70,false);}
        if(!op.skip)return finish(op.final_result);op.kind="file_open";op.phase=0;
    }
    if(op.kind=="file_open"){
        if(!op.phase++){
            const auto entry=op.floating?(x86_?"?_Fiopen@std@@YAPAU_iobuf@@PB_WHH@Z":"?_Fiopen@std@@YAPEAU_iobuf@@PEB_WHH@Z"):
                (x86_?"?_Fiopen@std@@YAPAU_iobuf@@PBDHH@Z":"?_Fiopen@std@@YAPEAU_iobuf@@PEBDHH@Z");
            return callback(resolve("msvcp90.dll",entry).value,{op.output,op.limit,op.count},71,false);
        }
        if(value){put(op.buffer+l.file_pointer(),value);memory.store(GuestAddress{op.buffer+l.file_pointer()-l.p},8,1);}
        if(op.ios){if(value)memory.store(GuestAddress{op.ios+l.state},32,0);else setstate(2);}
        return finish(value?op.final_result:op.service.symbol.starts_with("??0")?op.object:0);
    }
    if(op.kind=="file_close"){
        if(!op.phase++){
            if(!file){if(op.ios)setstate(2);return finish(0);}
            put(op.buffer+l.file_pointer(),0);memory.store(GuestAddress{op.buffer+l.file_pointer()-l.p},8,0);
            return crt("fclose",{file},72);
        }
        if(value && op.ios)setstate(2);return finish(op.ios?0:value?0:op.object);
    }
    if(op.kind=="file_destroy"){
        while(op.count<op.objects.size()){
            const auto root=op.objects[op.count],buffer=op.skip?root+2*l.p:root;
            if(!op.phase){op.phase=1;const auto owned=memory.load(GuestAddress{buffer+l.file_pointer()-l.p},8),stream=ptr(buffer+l.file_pointer());
                put(buffer+l.file_pointer(),0);if(owned && stream)return crt("fclose",{stream},73);}
            if(op.phase==1){op.phase=2;if(const auto lock=ptr(buffer+l.p))return callback(resolve("kernel32.dll","DeleteCriticalSection").value,{lock},74,false);}
            if(op.phase==2){op.phase=3;const auto lock=ptr(buffer+l.p),locale=ptr(buffer+l.buffer_locale());
                if(lock){(void)free_heap(memory,lock,state);put(buffer+l.p,0);}
                if(locale){const auto imp=ptr(locale),n=memory.load(GuestAddress{imp+l.p},l.reference_bits());if(n)memory.store(GuestAddress{imp+l.p},l.reference_bits(),n-1);
                    (void)free_heap(memory,locale,state);put(buffer+l.buffer_locale(),0);}
                if(op.unsigned_integer){const auto base=root+static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(memory.load(GuestAddress{ptr(root)+4},32))));
                    return callback(resolve("msvcp90.dll",x86_?"??1?$basic_ios@DU?$char_traits@D@std@@@std@@UAE@XZ":"??1?$basic_ios@DU?$char_traits@D@std@@@std@@UEAA@XZ").value,{base},75);}
            }
            ++op.count;op.phase=0;
        }
        if(op.output&3U)(void)free_heap(memory,op.object-((op.output&2U)?l.p:0),state);return finish(op.final_result);
    }
    if(op.kind=="file_read" || op.kind=="file_write"){
        if(returned)return finish(value);if(!file || !op.limit || op.limit>INT64_MAX)return finish(0);
        return crt(op.kind=="file_read"?"fread":"fwrite",{op.output,1,op.limit,file},76);
    }
    if(op.kind=="file_peek" || op.kind=="file_get"){
        if(!file)return finish(l.eof());
        if(!returned)return crt("fgetc",{file},77);
        if(op.kind=="file_get" || static_cast<std::uint32_t>(value)==UINT32_MAX)return finish(value);
        if(returned==77){op.count=value;return crt("ungetc",{value,file},78);}
        return finish(static_cast<std::uint32_t>(value)==UINT32_MAX?l.eof():op.count);
    }
    if(op.kind=="file_put" || op.kind=="file_unget"){
        if(!file)return finish(l.eof());if(returned)return finish(value);
        if(static_cast<std::uint32_t>(op.output)==UINT32_MAX){if(op.kind=="file_unget")return finish(l.eof());return crt("fflush",{file},79);}
        return crt(op.kind=="file_put"?"fputc":"ungetc",{op.output&255U,file},79);
    }
    if(op.kind=="file_seek"){
        const auto invalid=[&](){memory.fill(GuestAddress{op.output},0,24);memory.store(GuestAddress{op.output},l.stream_bits(),UINT64_MAX);return finish(op.output);};
        if(!file || op.count>2)return invalid();
        if(!returned)return crt("_fseeki64",{file,op.limit,op.count},80,x86_?2U:0U);
        if(value)return invalid();
        if(returned==80)return crt("fgetpos",{file,op.scratch},81);
        memory.fill(GuestAddress{op.output},0,24);memory.store(GuestAddress{op.output+8},64,memory.load(GuestAddress{op.scratch},64));return finish(op.output);
    }
    if(op.kind=="file_sync"){if(!file)return finish(UINT32_MAX);return returned?finish(value):crt("fflush",{file},82);}
    if(op.kind=="file_setbuf"){
        if(!file)return finish(0);if(returned)return finish(value?0:op.object);
        return crt("setvbuf",{file,op.output,!op.output && !op.limit?4U:0U,op.limit},83);
    }
    throw HostCallFault("VC9 檔案串流接續狀態無效");
}
