// 呼叫端已區分 GDI handle、客體磁碟與 IStream；三種來源在此共用格式辨識與所有權轉移。
std::uint32_t WindowsServices::read_gdiplus_image(GuestState& state,GuestMemory& memory,
        std::span<const std::uint8_t> bytes,std::uint64_t output,std::string_view operation,
        std::uint64_t source_handle,bool take_ownership,std::uint64_t placeable){
    const auto bits=static_cast<std::uint8_t>(x86_?32:64);
    const bool header=operation.starts_with("GdipGetMetafileHeader");
    const bool wants_meta=header || operation.starts_with("GdipCreateMetafile");
    const bool placeable_file=bytes.size()>=22 && emf_get(bytes,0)==0x9ac6cdd7U;
    const bool emf=bytes.size()>=44 && emf_get(bytes,0)==1 && emf_get(bytes,40)==0x464d4520;
    if(!emf && !placeable_file && !valid_windows_metafile(bytes)){
        if(wants_meta)return generic_error;
        return decode_bitmap(*gdiplus_,memory,bytes,output,bits,[&](auto n){return allocate_heap(memory,n);},
                             [&](auto at){(void)free_heap(memory,at,state);});
    }
    if(operation.starts_with("GdipCreateBitmap"))return generic_error;
    memory.check(GuestAddress{output},header?sizeof(gdiplus_meta::Header):bits/8,Access::Write);
    std::uint64_t temporary_emf=0,image_handle=0;
    const auto cleanup=[&]{
        if(temporary_emf)gui_.metafiles.erase(temporary_emf);
        if(image_handle)(void)free_heap(memory,image_handle,state);
    };
    try{
        gdiplus_meta::Image image{};
        if(emf){
            // 既有 handle 保留原始身分；從檔案或串流建立的 handle 歸 GpMetafile 所有。
            if(source_handle)image=gdiplus_meta::emf_image(gui_.metafiles.at(source_handle).bytes);
            else{
                std::vector<std::uint8_t> copy(bytes.begin(),bytes.end());
                image=gdiplus_meta::emf_image(copy);
                if(!header){
                    temporary_emf=import_enhanced_metafile(std::move(copy)).value_or(0);
                    if(!temporary_emf)return generic_error;
                }
            }
            image.emf=source_handle?source_handle:temporary_emf;
            image.own_emf=!source_handle || take_ownership;
        }else{
            std::array<std::uint8_t,22> place{};
            auto frame=placeable_file?bytes.first(22):std::span<const std::uint8_t>{};
            if(placeable_file)bytes=bytes.subspan(22);
            if(placeable){memory.read(GuestAddress{placeable},place);frame=place;}
            if(!valid_windows_metafile(bytes))return generic_error;
            bytes=bytes.first(windows_metafile_size(bytes));
            GuestScratch scratch(*this,memory,state);const auto buffer=scratch.allocate(bytes.size());
            if(!buffer)return out_of_memory;
            memory.write(GuestAddress{buffer},bytes);
            // WMF 邊界及轉交的 HENHMETAFILE 使用 GDI 的同一套轉檔與繪圖記錄。
            temporary_emf=nested_service(state,memory,"gdi32.dll","SetWinMetaFileBits",{bytes.size(),buffer,0,0});
            if(!temporary_emf)return generic_error;
            image=gdiplus_meta::emf_image(gui_.metafiles.at(temporary_emf).bytes);
            gdiplus_meta::set_wmf_header(image,bytes,frame);
            image.emf=temporary_emf;image.own_emf=true;
            // 裸 WMF 由影像解碼器載入時回報 EMF 格式；WMF handle 與 placeable 保留 WMF。
            if(source_handle || placeable_file || operation.ends_with("WmfFile"))image.format=0xad;
            image.wmf=source_handle;image.own_wmf=source_handle && take_ownership;
        }
        if(header){
            memory.write(GuestAddress{output},std::bit_cast<std::array<std::uint8_t,sizeof(image.header)>>(image.header));
            cleanup();return ok;
        }
        image_handle=allocate_heap(memory,16);
        if(!image_handle){cleanup();return out_of_memory;}
        gdiplus_->metafiles.emplace(image_handle,image);
        memory.store(GuestAddress{output},bits,image_handle);
        return ok;
    }catch(const std::invalid_argument&){cleanup();return generic_error;}
    catch(...){if(image_handle)gdiplus_->metafiles.erase(image_handle);cleanup();throw;}
}
