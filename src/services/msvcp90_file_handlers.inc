// VC9 窄字元 filebuf／ifstream：物件 ABI 留在此層，I/O 委派給共用 CRT。
if(l.legacy && (name.find("basic_ifstream@")!=name.npos || name.find("basic_filebuf@")!=name.npos || method.starts_with("File"))){
    const bool stream=name.find("basic_ifstream@")!=name.npos || method=="FileIosDelete";
    const bool construct=name.starts_with("??0") || name.starts_with("??_F");
    const bool destroy=name.starts_with("??1") || name.starts_with("??_D") || method=="FileDelete" || method=="FileIosDelete";
    const auto ios_offset=2*l.p+l.file_size();
    auto root=object;
    if(stream && (name.starts_with("??1") || method=="FileIosDelete")){
        const auto found=s.ios_objects.find(object);if(found==s.ios_objects.end())throw HostCallFault("VC9 ifstream 解構物件無效");root=found->second.first;
    }
    const auto file_buffer=stream?root+2*l.p:root;
    const auto ios=stream && !construct?root+static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(memory.load(GuestAddress{ptr(root)+4},32)))):0;
    if(method=="rdbuf")return done(file_buffer);
    if(method=="is_open")return done(ptr(file_buffer+l.file_pointer())!=0);
    Operation op;op.service=service;op.element=1;op.object=root;op.buffer=file_buffer;op.ios=ios;
    if(construct){
        const bool path=name.find("@PBD")!=name.npos || name.find("@PEBD")!=name.npos || name.find("@PBG")!=name.npos || name.find("@PEBG")!=name.npos || name.find("@PB_W")!=name.npos || name.find("@PEB_W")!=name.npos;
        const bool existing=name.find("_iobuf")!=name.npos;
        if(stream){
            if(a(path?3:existing?1:0))put(root,s.ifstream_vb9);
            op.ios=root+static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(memory.load(GuestAddress{ptr(root)+4},32))));
            memory.fill(GuestAddress{op.ios},0,l.ios_size+3*l.p);put(op.ios,s.ifstream_table9);
            memory.store(GuestAddress{op.ios+l.flags},32,0x201);memory.store(GuestAddress{op.ios+l.precision},l.stream_bits(),6);
            put(op.ios+l.locale,new_locale());put(op.ios+l.ios_size,file_buffer);memory.store(GuestAddress{op.ios+l.ios_size+2*l.p},8,' ');
            memory.store(GuestAddress{root+l.count_offset()},l.stream_bits(),0);
            s.ios_objects[op.ios]={root,op.ios-root+l.ios_size+3*l.p};
        }
        memory.fill(GuestAddress{file_buffer},0,l.file_size());put(file_buffer,s.file_table9);put(file_buffer+l.buffer_locale(),new_locale());
        const auto lock=allocate_heap(memory,x86_?24:40);if(!lock)throw std::bad_alloc{};put(file_buffer+l.p,lock);
        for(unsigned side=0;side<2;++side){put(file_buffer+l.first_indirect(side),file_buffer+l.first(side));put(file_buffer+l.next_indirect(side),file_buffer+l.next(side));put(file_buffer+l.count_indirect(side),file_buffer+l.count(side));}
        if(existing)put(file_buffer+l.file_pointer(),a(0));
        op.kind="file_construct";op.skip=path;op.output=path?a(0):0;op.limit=path?(a(1)|(stream?1U:0U)):0;op.count=path?a(2):0;
        op.floating=path && name.find("PBD")==name.npos && name.find("PEBD")==name.npos;op.final_result=root;
    }else if(destroy){
        op.kind="file_destroy";op.skip=stream;op.unsigned_integer=stream && !name.starts_with("??1");op.ios=0;
        op.output=method=="FileDelete" || method=="FileIosDelete"?a(0):0;op.final_result=method=="FileDelete" || method=="FileIosDelete"?root:0;
        if(op.output&2U){const auto n=ptr(root-l.p);if(n>1048576)throw HostCallFault("VC9 串流陣列長度無效");
            const auto size=stream?ios_offset+l.ios_size+3*l.p:l.file_size();for(auto i=n;i>0;--i)op.objects.push_back(root+(i-1)*size);
        }else op.objects.push_back(root);
    }else if(method=="open"){
        if(ptr(file_buffer+l.file_pointer())){if(ios){const auto at=ios+l.state;memory.store(GuestAddress{at},32,memory.load(GuestAddress{at},32)|2U);
                if(memory.load(GuestAddress{at},32)&memory.load(GuestAddress{at+4},32)){throw_msvc_exception(state,memory,service,MsvcExceptionKind::IosFailure);return {};}}
            return done(0);}
        op.kind="file_open";op.output=a(0);op.limit=a(1)|(stream?1U:0U);op.count=a(2);op.floating=name.find("PBD")==name.npos && name.find("PEBD")==name.npos;op.final_result=stream?0:root;
    }else if(method=="close")op.kind="file_close";
    else if(method=="FileXsgetn" || method=="FileXsgetnSafe" || method=="FileXsputn"){
        op.kind=method=="FileXsputn"?"file_write":"file_read";op.output=a(0);op.limit=a(method=="FileXsgetnSafe"?2:1);
        if(l.stream_bits()==32)op.limit=static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(op.limit)));
        if(method=="FileXsgetnSafe" && op.limit<=INT64_MAX && op.limit>a(1)){
            if(a(1) && a(0))memory.fill(GuestAddress{a(0)},0,static_cast<std::size_t>(a(1)));
            memory.store(GuestAddress{runtime_.errno_cell},32,34);if(current_invalid_parameter_handler()){start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,0);return {};}
            return done(0);
        }
    }else if(method=="FileUnderflow" || method=="FileUflow" || method=="FileOverflow" || method=="FilePbackfail"){
        op.kind=method=="FileUnderflow"?"file_peek":method=="FileUflow"?"file_get":method=="FileOverflow"?"file_put":"file_unget";op.output=method=="FileOverflow" || method=="FilePbackfail"?a(0):0;
    }else if(method=="FileSeekoff" || method=="FileSeekpos"){
        op.kind="file_seek";op.output=a(0);op.scratch=allocate_heap(memory,24);if(!op.scratch)throw std::bad_alloc{};
        if(method=="FileSeekoff"){op.limit=a(1);if(l.stream_bits()==32)op.limit=static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(op.limit)));op.count=a(2);}
        else {const auto position=x86_?state.reg(Gpr::Rsp)+8:a(1);const auto offset=memory.load(GuestAddress{position},l.stream_bits());
            op.limit=memory.load(GuestAddress{position+8},64)+(l.stream_bits()==32?static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(offset))):offset);op.count=0;}
    }else if(method=="FileSync")op.kind="file_sync";
    else if(method=="FileSetbuf"){op.kind="file_setbuf";op.output=a(0);op.limit=a(1);}
    else if(method=="FileImbue"){
        // char 的預設 codecvt 不轉碼；locale 物件仍保存並管理引用，供 getloc 使用。
        const auto replacement=ptr(a(0)),loc=ptr(file_buffer+l.buffer_locale()),old=ptr(loc);
        if(replacement!=old){put_refs(replacement,refs(replacement)+1);put(loc,replacement);const auto n=refs(old);if(n)put_refs(old,n-1);}return done(0);
    }else throw HostCallFault("未知 VC9 檔案串流操作");
    const auto token=s.next++;s.operations.emplace(token,std::move(op));return resume_msvcp100(state,memory,token,0);
}
