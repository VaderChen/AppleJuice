// mfplat 的 MFT 登錄與列舉（由 windows_media_foundation.cpp 的匯出鏈引入）：
// - MFTRegister／MFTUnregister／MFTGetInfo：HKLM\SOFTWARE\Classes\MediaFoundation\Transforms\<clsid>（預設值為名稱，InputTypes／OutputTypes 為
//   MFT_REGISTER_TYPE_INFO 陣列，MFTFlags，Attributes 為 MFGetAttributesAsBlob 的格式），分類索引在 Transforms\Categories\<分類>\<clsid>；
//   GUID 以小寫、不含大括號的字串表示。經 advapi32 的登錄服務讀寫，權限與持久化沿用沙箱登錄。
// - MFTRegisterLocal／MFTRegisterLocalByCLSID、MFTUnregisterLocal／MFTUnregisterLocalByCLSID：只在本程序有效。
// - MFTEnum：登錄的同步 MFT 的 CLSID 陣列。MFTEnumEx／MFTEnum2：依序列出本機登錄（需 MFT_ENUM_FLAG_LOCALMFT）、硬體（VideoToolbox H.264）與登錄的 MFT，
//   依同步／非同步／硬體／field-of-use／轉碼專用旗標與輸入、輸出型別篩選（子型別 GUID_NULL 視為任一）。IMFActivate::ActivateObject 以 CoCreateInstance
//   或本機登錄的類別工廠建立（windows_media_foundation.cpp 開頭的協程）。MFTEnum2 只有一張顯示卡，屬性中的 MFT_ENUM_ADAPTER_LUID 不影響結果。
if(name=="MFTRegister" || name=="MFTUnregister" || name=="MFTGetInfo" || name=="MFTRegisterLocal" || name=="MFTRegisterLocalByCLSID" ||
   name=="MFTUnregisterLocal" || name=="MFTUnregisterLocalByCLSID" || name=="MFTEnum" || name=="MFTEnumEx" || name=="MFTEnum2"){
    const std::uint64_t machine=x86_?std::uint64_t{0x80000002U}:UINT64_C(0xffffffff80000002);
    const std::u16string root=u"SOFTWARE\\Classes\\MediaFoundation\\Transforms",categories=root+u"\\Categories";
    const Guid flags_key=mf::guid(0x9359bb7e,0x6275,0x46c4,{0xa0,0x25,0x1c,0x01,0xe4,0x5f,0x1a,0x86}),category_key=mf::guid(0xceabba49,0x506d,0x4757,{0xa6,0xff,0x66,0xc1,0x84,0x98,0x7e,0x4e});
    constexpr std::uint32_t not_found=0x80070490U,file_not_found=0x80070002U;
    const auto text_of=[](const Guid& id){
        static constexpr char16_t digits[]=u"0123456789abcdef";std::u16string out;
        const auto hex=[&](std::uint8_t value){out.push_back(digits[value>>4]);out.push_back(digits[value&15]);};
        for(const unsigned i:{3U,2U,1U,0U})hex(id[i]);out+=u'-';hex(id[5]);hex(id[4]);out+=u'-';hex(id[7]);hex(id[6]);out+=u'-';hex(id[8]);hex(id[9]);out+=u'-';
        for(unsigned i=10;i<16;++i)hex(id[i]);return out;};
    const auto parse=[](std::u16string_view text)->std::optional<Guid>{
        if(text.size()==38 && text.front()==u'{' && text.back()==u'}')text=text.substr(1,36);
        if(text.size()!=36)return std::nullopt;std::array<std::uint8_t,32> nibbles{};unsigned count=0;
        for(std::size_t i=0;i<36;++i){
            const auto c=text[i];if(i==8 || i==13 || i==18 || i==23){if(c!=u'-')return std::nullopt;continue;}
            if(c>=u'0' && c<=u'9')nibbles[count++]=static_cast<std::uint8_t>(c-u'0');else if(c>=u'a' && c<=u'f')nibbles[count++]=static_cast<std::uint8_t>(c-u'a'+10);
            else if(c>=u'A' && c<=u'F')nibbles[count++]=static_cast<std::uint8_t>(c-u'A'+10);else return std::nullopt;}
        const auto byte=[&](unsigned i){return static_cast<std::uint8_t>(nibbles[2*i]<<4|nibbles[2*i+1]);};
        Guid id{};id[0]=byte(3);id[1]=byte(2);id[2]=byte(1);id[3]=byte(0);id[4]=byte(5);id[5]=byte(4);id[6]=byte(7);id[7]=byte(6);for(unsigned i=8;i<16;++i)id[i]=byte(i);return id;};
    const auto win32=[](std::uint32_t error){return error?0x80070000U|(error&0xffffU):0U;};
    const auto registry=[&](std::string_view symbol,std::vector<std::uint64_t> args){return static_cast<std::uint32_t>(nested_service(state,memory,"advapi32.dll",symbol,args));};
    // 開啟或建立的登錄機碼在範圍結束時關閉。
    struct Key {WindowsServices& self;GuestState& state;GuestMemory& memory;std::uint64_t handle{};
        ~Key(){if(handle)(void)self.nested_service(state,memory,"advapi32.dll","RegCloseKey",{handle});}};
    const auto open_key=[&](Key& key,const std::u16string& path,bool create)->std::uint32_t{
        GuestScratch scratch(*this,memory,state);const auto text=scratch.text(path),slot=scratch.allocate(8);require(text && slot,0x8007000eU);
        const auto error=create?registry("RegCreateKeyExW",{machine,text,0,0,0,0xf003f,0,slot,0}):registry("RegOpenKeyExW",{machine,text,0,0x20019,slot});
        key.handle=error?0:memory.load(GuestAddress{slot},bits);return error;};
    const auto delete_tree=[&](const std::u16string& path){GuestScratch scratch(*this,memory,state);const auto text=scratch.text(path);require(text!=0,0x8007000eU);return registry("RegDeleteTreeW",{machine,text});};
    const auto set_value=[&](const Key& key,std::u16string_view value_name,std::uint32_t type,std::span<const std::uint8_t> bytes){
        GuestScratch scratch(*this,memory,state);const auto text=value_name.empty()?0:scratch.text(value_name);const auto buffer=scratch.allocate(bytes.size());require(buffer!=0,0x8007000eU);
        if(!bytes.empty())memory.write(GuestAddress{buffer},bytes);const auto error=registry("RegSetValueExW",{key.handle,text,0,type,buffer,bytes.size()});require(!error,win32(error));};
    const auto query_value=[&](const Key& key,std::u16string_view value_name)->std::vector<std::uint8_t>{
        GuestScratch scratch(*this,memory,state);const auto text=value_name.empty()?0:scratch.text(value_name);const auto size=scratch.allocate(4);require(size!=0,0x8007000eU);
        if(registry("RegQueryValueExW",{key.handle,text,0,0,0,size}))return {};const auto length=static_cast<std::uint32_t>(memory.load(GuestAddress{size},32));
        require(length<=16U*1024U*1024U);const auto buffer=scratch.allocate(length);require(buffer!=0,0x8007000eU);
        if(registry("RegQueryValueExW",{key.handle,text,0,0,buffer,size}))return {};return data(buffer,static_cast<std::uint32_t>(memory.load(GuestAddress{size},32)));};
    const auto subkeys=[&](const Key& key){
        std::vector<std::u16string> names;GuestScratch scratch(*this,memory,state);const auto buffer=scratch.allocate(2*260),length=scratch.allocate(4);require(buffer && length,0x8007000eU);
        for(std::uint32_t index=0;index<65536;++index){memory.store(GuestAddress{length},32,260);if(registry("RegEnumKeyExW",{key.handle,index,buffer,length,0,0,0,0}))break;names.push_back(read_utf16(memory,GuestAddress{buffer},260));}
        return names;};
    // MFT_REGISTER_TYPE_INFO 陣列：每筆 {主要型別, 子型別}。
    const auto type_infos=[&](std::uint32_t count,std::uint64_t pointer){require(count<=4096);require(!count || pointer!=0,pointer_error);return data(pointer,static_cast<std::size_t>(count)*32);};
    using Transform=MediaFoundationState::LocalTransform;
    const auto load_registered=[&](const Guid& clsid,const Guid& category)->std::optional<Transform>{
        Key key{*this,state,memory};if(open_key(key,root+u"\\"+text_of(clsid),false))return std::nullopt;
        Transform loaded;loaded.clsid=clsid;loaded.category=category;
        const auto name_bytes=query_value(key,u"");for(std::size_t i=0;i+1<name_bytes.size();i+=2){const auto c=static_cast<char16_t>(name_bytes[i]|name_bytes[i+1]<<8);if(!c)break;loaded.name.push_back(c);}
        loaded.inputs=query_value(key,u"InputTypes");loaded.outputs=query_value(key,u"OutputTypes");loaded.attributes=query_value(key,u"Attributes");
        if(const auto flags=query_value(key,u"MFTFlags");flags.size()>=4)loaded.flags=static_cast<std::uint32_t>(fmt_le(flags,0,4));
        loaded.inputs.resize(loaded.inputs.size()/32*32);loaded.outputs.resize(loaded.outputs.size()/32*32);return loaded;};
    const auto type_matches=[&](const std::vector<std::uint8_t>& types,std::uint64_t filter){
        if(!filter)return true;const auto wanted=data(filter,32);
        for(std::size_t at=0;at+32<=types.size();at+=32){
            const bool major=std::equal(types.begin()+static_cast<std::ptrdiff_t>(at),types.begin()+static_cast<std::ptrdiff_t>(at+16),wanted.begin());
            const bool any=std::all_of(types.begin()+static_cast<std::ptrdiff_t>(at+16),types.begin()+static_cast<std::ptrdiff_t>(at+32),[](std::uint8_t b){return b==0;}) || std::all_of(wanted.begin()+16,wanted.end(),[](std::uint8_t b){return b==0;});
            if(major && (any || std::equal(types.begin()+static_cast<std::ptrdiff_t>(at+16),types.begin()+static_cast<std::ptrdiff_t>(at+32),wanted.begin()+16)))return true;}
        return false;};
    // 登錄旗標：ASYNCMFT 2、HARDWARE 4、FIELDOFUSE 8、TRANSCODE_ONLY 0x20；要求旗標需涵蓋 MFT 本身的種類。
    const auto flags_match=[](std::uint32_t registered,std::uint32_t requested){
        if((registered&4)?!(requested&4):(registered&2)?!(requested&2):!(requested&1))return false;
        return !((registered&8) && !(requested&8)) && !((registered&0x20) && !(requested&0x20));};
    const auto activation_for=[&](const Transform& source){
        const auto p=make(mf::Activate);auto activation=object(p,mf::Activate);activation->identifier=source.factory?7:6;
        try{
            if(source.factory){hold(source.factory);activation->callback=source.factory;}
            if(!source.attributes.empty())if(const auto items=decode_attributes(source.attributes))for(const auto& [key,value]:*items)if(value.type!=13)set(*activation,key,value);
            set(*activation,mf::MFT_TRANSFORM_CLSID_Attribute,valueguid(source.clsid));if(!source.name.empty())set(*activation,mf::MFT_FRIENDLY_NAME_Attribute,valuestring(source.name));
            set(*activation,flags_key,value32(source.flags));set(*activation,category_key,valueguid(source.category));if(source.flags&2)set(*activation,mf::MF_TRANSFORM_ASYNC,value32(1));
            for(const auto& [key,types]:std::array<std::pair<Guid,const std::vector<std::uint8_t>*>,2>{{{mf::MFT_INPUT_TYPES_Attributes,&source.inputs},{mf::MFT_OUTPUT_TYPES_Attributes,&source.outputs}}})
                if(!types->empty()){Value value;value.type=0x1011;value.blob=*types;set(*activation,key,std::move(value));}
        }catch(...){release(p);throw;}
        return p;};
    // MFT_ENUM_FLAG_SORTANDFILTER（0x40）時排除 IMFPluginControl 停用的 MFT（MF_Plugin_Type_MFT＝0）。
    const auto disabled=[&](const Guid& clsid,std::uint32_t requested){
        return (requested&0x40) && std::find(bridge.plugins.disabled.begin(),bridge.plugins.disabled.end(),std::pair<std::uint32_t,Guid>{0,clsid})!=bridge.plugins.disabled.end();};
    const auto local_text=[&](std::uint64_t pointer){return pointer?read_utf16(memory,GuestAddress{pointer},1024):std::u16string{};};
    if(name=="MFTRegister"){
        // (clsidMFT, guidCategory, pszName, Flags, cInputTypes, pInputTypes, cOutputTypes, pOutputTypes, pAttributes)
        const auto clsid=guid(arg(0)),category=guid(arg(1));const auto inputs=type_infos(d(4),arg(5)),outputs=type_infos(d(6),arg(7));
        std::vector<std::uint8_t> attributes;if(arg(8)){auto store=entry(arg(8)).object;require(mf::attributes(store->kind),no_interface);attributes=encode_attributes(store->attributes,false);}
        const auto path=root+u"\\"+text_of(clsid);(void)delete_tree(path);
        {Key key{*this,state,memory};const auto error=open_key(key,path,true);require(!error,win32(error));
            if(arg(2)){const auto bytes=utf16_bytes(local_text(arg(2)));set_value(key,u"",1,bytes);}
            if(!inputs.empty())set_value(key,u"InputTypes",3,inputs);if(!outputs.empty())set_value(key,u"OutputTypes",3,outputs);
            if(d(3)){std::vector<std::uint8_t> flags(4);fmt_put(flags,0,d(3),4);set_value(key,u"MFTFlags",4,flags);}
            if(!attributes.empty())set_value(key,u"Attributes",3,attributes);}
        Key index{*this,state,memory};const auto error=open_key(index,categories+u"\\"+text_of(category)+u"\\"+text_of(clsid),true);require(!error,win32(error));
    }else if(name=="MFTUnregister"){
        const auto clsid=guid(arg(0));const auto error=delete_tree(root+u"\\"+text_of(clsid));
        bool indexed=false;
        {Key key{*this,state,memory};if(!open_key(key,categories,false))for(const auto& category:subkeys(key))if(!delete_tree(categories+u"\\"+category+u"\\"+text_of(clsid)))indexed=true;}
        require(!error || indexed,error==2?file_not_found:win32(error));
    }else if(name=="MFTGetInfo"){
        // (clsidMFT, pszName, ppInputTypes, pcInputTypes, ppOutputTypes, pcOutputTypes, ppAttributes)：各輸出皆可為 NULL，配置以 CoTaskMemFree 釋放。
        for(const unsigned at:{1U,2U,4U,6U})if(arg(at))put(arg(at),0,bits);for(const unsigned at:{3U,5U})if(arg(at))put(arg(at),0);
        const auto found=load_registered(guid(arg(0)),Guid{});require(found.has_value(),file_not_found);
        std::vector<std::uint64_t> blocks;
        const auto publish=[&](std::span<const std::uint8_t> bytes){const auto p=allocate(bytes.size());blocks.push_back(p);if(!bytes.empty())memory.write(GuestAddress{p},bytes);gui_.task_allocations.insert(p);return p;};
        try{
            if(arg(1)){auto text=utf16_bytes(found->name);put(arg(1),publish(text),bits);}
            if(arg(2) && arg(3)){put(arg(2),found->inputs.empty()?0:publish(found->inputs),bits);put(arg(3),found->inputs.size()/32);}
            if(arg(4) && arg(5)){put(arg(4),found->outputs.empty()?0:publish(found->outputs),bits);put(arg(5),found->outputs.size()/32);}
            if(arg(6)){const auto p=make(mf::Attributes);if(!found->attributes.empty())if(const auto items=decode_attributes(found->attributes))for(const auto& [key,value]:*items)if(value.type!=13)set(*object(p,mf::Attributes),key,value);put(arg(6),p,bits);}
        }catch(...){for(const auto p:blocks){gui_.task_allocations.erase(p);(void)free_heap(memory,p,state);}throw;}
    }else if(name=="MFTRegisterLocal" || name=="MFTRegisterLocalByCLSID"){
        // (pClassFactory 或 clsidMFT, guidCategory, pszName, Flags, cInputTypes, pInputTypes, cOutputTypes, pOutputTypes)
        const bool by_clsid=name.ends_with("CLSID");require(arg(0)!=0 && arg(1)!=0,pointer_error);require(bridge.local_transforms.size()<4096,0x8007000eU);
        Transform local;local.category=guid(arg(1));local.name=local_text(arg(2));local.flags=d(3);local.inputs=type_infos(d(4),arg(5));local.outputs=type_infos(d(6),arg(7));
        if(by_clsid)local.clsid=guid(arg(0));else {(void)foreign_method(arg(0),3);local.factory=arg(0);hold(arg(0));}
        bridge.local_transforms.push_back(std::move(local));
    }else if(name=="MFTUnregisterLocal" || name=="MFTUnregisterLocalByCLSID"){
        // MFTUnregisterLocal(NULL) 解除全部本機登錄；找不到時回報 HRESULT_FROM_WIN32(ERROR_NOT_FOUND)。
        const bool by_clsid=name.ends_with("CLSID");const auto clsid=by_clsid?guid(arg(0)):Guid{};std::size_t removed=0;
        for(auto it=bridge.local_transforms.begin();it!=bridge.local_transforms.end();){
            const bool match=by_clsid?(!it->factory && it->clsid==clsid):(!arg(0) || (it->factory && it->factory==arg(0)));
            if(!match){++it;continue;}if(it->factory)release(it->factory);it=bridge.local_transforms.erase(it);++removed;}
        require(removed || (!by_clsid && !arg(0)),not_found);
    }else {
        // MFTEnum(guidCategory, Flags, pInputType, pOutputType, pAttributes, ppclsidMFT, pcMFTs)
        // MFTEnumEx(guidCategory, Flags, pInputType, pOutputType, pppMFTActivate, pnumMFTActivate)
        // MFTEnum2(guidCategory, Flags, pInputType, pOutputType, pAttributes, pppMFTActivate, pnumMFTActivate)
        const bool legacy=name=="MFTEnum",second=name=="MFTEnum2";const auto output_arg=legacy || second?5U:4U,count_arg=output_arg+1;
        out(arg(output_arg));require(arg(count_arg)!=0,pointer_error);put(arg(count_arg),0);const auto category=guid(arg(0));auto flags=d(1);
        if(second && arg(4)){auto store=entry(arg(4)).object;require(mf::attributes(store->kind),no_interface);}
        // 舊列舉僅包含同步軟體 MFT；共用實際可用的目錄與型別篩選，不能把非同步硬體解碼器列入。
        if(legacy){require(flags==0);flags=1;}else {require(!(flags&~0x1ffU));if(!flags)flags=0x51;if(!(flags&7))flags|=1;}
        std::vector<Transform> registered;
        {Key key{*this,state,memory};if(!open_key(key,categories+u"\\"+text_of(category),false))
            for(const auto& text:subkeys(key))if(const auto clsid=parse(text))if(auto found=load_registered(*clsid,category))
                if(flags_match(found->flags,flags) && type_matches(found->inputs,arg(2)) && type_matches(found->outputs,arg(3)) && !disabled(found->clsid,flags))registered.push_back(std::move(*found));}
        if(legacy){
            if(!registered.empty()){
                std::vector<std::uint8_t> bytes;for(const auto& found:registered)bytes.insert(bytes.end(),found.clsid.begin(),found.clsid.end());
                const auto p=allocate(bytes.size());memory.write(GuestAddress{p},bytes);gui_.task_allocations.insert(p);put(arg(output_arg),p,bits);put(arg(count_arg),registered.size());
            }
        }else {
            std::vector<std::uint64_t> found;
            try{
                if(flags&0x10)for(const auto& local:bridge.local_transforms)
                    if(local.category==category && flags_match(local.flags,flags) && type_matches(local.inputs,arg(2)) && type_matches(local.outputs,arg(3)) && !(local.clsid!=Guid{} && disabled(local.clsid,flags)))found.push_back(activation_for(local));
                const auto matches=[&](std::uint64_t filter,const Guid& subtype){return !filter || (guid(filter)==mf::MFMediaType_Video && guid(filter+16)==subtype);};
                const bool encode=category==mf::MFT_CATEGORY_VIDEO_ENCODER;
                const auto input_subtype=encode?mf::nv12:mf::h264,output_subtype=encode?mf::h264:mf::nv12;
                if((encode || category==mf::MFT_CATEGORY_VIDEO_DECODER) && (flags&4) && matches(arg(2),input_subtype) && matches(arg(3),output_subtype) &&
                    (encode?video::hardware_encode_supported(video::Codec::H264):video::hardware_decode_supported(video::Codec::H264))){
                    const auto p=make(mf::Activate);found.push_back(p);auto activation=object(p,mf::Activate);activation->identifier=encode?3:1;
                    set(*activation,mf::MFT_TRANSFORM_CLSID_Attribute,valueguid(encode?mf::native_h264_encoder:mf::native_h264));set(*activation,mf::MFT_FRIENDLY_NAME_Attribute,valuestring(encode?u"MacWin VideoToolbox H.264 硬體編碼器":u"MacWin VideoToolbox H.264 硬體解碼器"));
                    set(*activation,mf::MF_TRANSFORM_ASYNC,value32(1));set(*activation,mf::MFT_ENUM_HARDWARE_URL_Attribute,valuestring(encode?u"macwin://videotoolbox/h264/encoder":u"macwin://videotoolbox/h264"));set(*activation,mf::MFT_ENUM_HARDWARE_VENDOR_ID_Attribute,valuestring(u"VEN_106B"));
                    set(*activation,flags_key,value32(6));set(*activation,category_key,valueguid(category));
                    for(const auto& [key,subtype]:std::array<std::pair<Guid,Guid>,2>{{{mf::MFT_INPUT_TYPES_Attributes,input_subtype},{mf::MFT_OUTPUT_TYPES_Attributes,output_subtype}}}){Value value;value.type=0x1011;value.blob.insert(value.blob.end(),mf::MFMediaType_Video.begin(),mf::MFMediaType_Video.end());value.blob.insert(value.blob.end(),subtype.begin(),subtype.end());set(*activation,key,std::move(value));}
                }
                for(const auto& entry_value:registered)found.push_back(activation_for(entry_value));
                if(!found.empty()){
                    const auto array=allocate(found.size()*unit);for(std::size_t i=0;i<found.size();++i)put(array+i*unit,found[i],bits);gui_.task_allocations.insert(array);
                    put(arg(output_arg),array,bits);put(arg(count_arg),found.size());
                }
            }catch(...){for(const auto p:found)release(p);throw;}
        }
    }
}
