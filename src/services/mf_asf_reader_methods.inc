// 多串流 ASF 來源的 Source Reader 方法；位於 L_ 方法共用流程內（item 為 Reader、source 為其來源）。
// 輸出型別為原生壓縮型別時原樣交付；要求其他型別時由 asf_decode_output 建立解碼輸出。
const auto& asf_file=source->file_source;const auto& asf_streams=asf_file->asf->streams;
const auto asf_descriptor=[&](std::size_t index){return object(object(source->children.at(0),mf::Presentation)->children.at(index),mf::Stream);};
// 串流索引：實際索引、第一條視訊（0xFFFFFFFC）、第一條音訊（0xFFFFFFFD）；允許時 0xFFFFFFFE 表示任一／全部串流（回傳 UINT32_MAX）。
const auto asf_index=[&](unsigned n,bool any=false)->std::uint32_t{
    const auto value=d(n);
    if(value==0xfffffffcU || value==0xfffffffdU){
        const auto found=std::find_if(asf_streams.begin(),asf_streams.end(),[&](const mf::AsfStream& stream){return value==0xfffffffcU?stream.video:stream.audio;});
        require(found!=asf_streams.end(),0xc00d36b3U);return static_cast<std::uint32_t>(found-asf_streams.begin());
    }
    if(any && value==0xfffffffeU)return UINT32_MAX;
    require(value<asf_streams.size(),0xc00d36b3U);return value;
};
const auto asf_idle=[&]{require(!item->request_pending && !item->file_read_active,0xc00d36b2U);};
if(name=="L_GetStreamSelection"){const auto index=asf_index(1);put(arg(2),item->selected.at(index));}
else if(name=="L_SetStreamSelection"){
    const auto index=asf_index(1,true);asf_idle();auto selected=item->selected;
    if(index==UINT32_MAX)selected.assign(selected.size(),d(2)!=0);else selected[index]=d(2)!=0;
    asf_select(asf_file,selected);item->selected=std::move(selected);
}else if(name=="L_GetNativeMediaType" || name=="L_GetCurrentMediaType"){
    const bool native=name=="L_GetNativeMediaType";out(arg(native?3:2));const auto index=asf_index(1);if(native)require(d(2)==0,0xc00d36b9U);
    // 交回複本：呼叫端修改取得的型別不影響來源的原生型別與 Reader 的目前型別。
    put(arg(native?3:2),copy_media_type(native?asf_descriptor(index)->children.at(0):item->children.at(index)),bits);
}else if(name=="L_SetCurrentMediaType"){
    const auto index=asf_index(1);require(!arg(2));asf_idle();
    const auto native_pointer=asf_descriptor(index)->children.at(0);const auto type=object(arg(3),mf::MediaType),native=object(native_pointer,mf::MediaType);
    require(get(*type,mf::MF_MT_MAJOR_TYPE)==get(*native,mf::MF_MT_MAJOR_TYPE),0xc00d36b4U);
    // 原生串流的原樣交付輸出（視訊附帶容器的平均每幀時間）。
    const auto passthrough=[&](std::size_t n){auto output=std::make_unique<AsfStreamOutput>();if(asf_streams.at(n).video)output->frame_time=asf_streams[n].frame_time;return output;};
    std::uint64_t p=0;std::unique_ptr<AsfStreamOutput> output;
    if(!(get(*type,mf::MF_MT_SUBTYPE)==get(*native,mf::MF_MT_SUBTYPE)))p=asf_decode_output(asf_streams.at(index),*type,output);
    else{
        for(const auto& [key,value]:type->attributes)if(native->attributes.contains(key))require(native->attributes.at(key)==value,0xc00d36b4U);
        p=copy_media_type(native_pointer);for(const auto& [key,value]:type->attributes)set(*object(p,mf::MediaType),key,value);output=passthrough(index);
    }
    // 第一次要求解碼時建立整個 Reader 的輸出佇列，之後所有串流都經由佇列讀取（原生型別的串流原樣交付）。
    if(output->decoding() && !item->asf_decode){auto decode=std::make_shared<AsfReaderDecode>();for(std::size_t n=0;n<asf_streams.size();++n)decode->streams.push_back(passthrough(n));item->asf_decode=std::move(decode);}
    if(item->asf_decode)item->asf_decode->streams.at(index)=std::move(output);
    release(item->children.at(index));item->children.at(index)=p;
}else if(name=="L_SetCurrentPosition"){
    asf_idle();require(guid(arg(1))==Guid{},0xc00d36c5U);
    const auto position=read_value(arg(2));require(position.type==20 && std::bit_cast<std::int64_t>(position.scalar)>=0);
    (void)asf_io(asf_file,AsfOp::Seek,0,std::bit_cast<std::int64_t>(position.scalar));
    if(item->asf_decode)for(auto& output:item->asf_decode->streams)output->reset(true);
}else if(name=="L_ReadSample"){
    const auto index=asf_index(1,true);require(!(d(2)&~1U));require(!item->flush_pending,0xc00d36b5U);require(!item->request_pending,0xc00d36b2U);
    require(index==UINT32_MAX?std::find(item->selected.begin(),item->selected.end(),true)!=item->selected.end():bool(item->selected.at(index)),0xc00d36b2U);
    if(item->callback){require(!arg(3) && !arg(4) && !arg(5) && !arg(6));source_start(source);item->request_stream=index;item->request_pending=true;hold_job(item);}
    else {
        out(arg(6));check(arg(4),4);if(arg(5))check(arg(5),8);if(arg(3))put(arg(3),0);put(arg(4),0);if(arg(5))put(arg(5),0,64);source_start(source);
        std::uint32_t actual=0;const auto p=item->asf_decode?asf_decoded_sample(item,source,index,actual):asf_sample(source,index,actual);if(arg(3))put(arg(3),actual);
        if(p){put(arg(6),p,bits);if(arg(5))put(arg(5),*object(p,mf::Sample)->time,64);}else put(arg(4),2);
    }
}else if(name=="L_Flush"){
    (void)asf_index(1,true);item->request_pending=false;++item->generation;
    // 丟棄已解多工但尚未取走的樣本與重組中的物件；讀取位置不變。讀取仍在進行時由其收尾通知。
    if(!asf_file->demux->busy){asf_file->demux->ready.clear();asf_file->demux->partial.clear();}
    // 解碼器與輸出佇列一併清除；解碼讀取仍在客體回呼中時，等該工作結束再清除。
    if(item->asf_decode){if(item->asf_decode->pump_pending)item->asf_decode->flush_requested=true;else for(auto& output:item->asf_decode->streams)output->reset(false);}
    if(item->file_read_active)item->flush_pending=true;else if(item->callback)callbacks.push_back({foreign_method(item->callback,4),{item->callback,0}});
}else if(name=="L_GetServiceForStream"){
    out(arg(4));require(d(1)==UINT32_MAX,0xc00d36b3U);require(guid(arg(2))==Guid{},0xc00d36d4U);query(item->parent,guid(arg(3)),arg(4));
}else if(name=="L_GetPresentationAttribute"){
    if(d(1)==UINT32_MAX && guid(arg(2))==mf::MF_SOURCE_READER_MEDIASOURCE_CHARACTERISTICS)write_value(arg(3),value32(6));
    else if(d(1)==UINT32_MAX)write_value(arg(3),get(*object(source->children.at(0),mf::Presentation),guid(arg(2))));
    else write_value(arg(3),get(*asf_descriptor(asf_index(1)),guid(arg(2))));
}else throw Failure{no_interface};
