// cfgmgr32 裝置查詢（devquery，由 windows_device_configuration.cpp 的 invoke_device_configuration 引入）：
// - DevCreateObjectQuery(Ex)：驗證與結果同 DevGetObjects（另接受 DevQueryFlagUpdateResults／AsyncClose），回呼由 internal-devquery Deliver 在執行緒池送出（見 windows_device_configuration.cpp）。
// - DevGetObjects(Ex)：依物件型別列出同一份 devnode 模型的物件——裝置（DevObjectTypeDevice，識別碼為執行個體 ID，含不存在的裝置，以 DEVPKEY_Device_IsPresent 區分）、
//   裝置介面（識別碼為符號連結）、介面類別與安裝程式類別（識別碼為大寫 {GUID}，沒有屬性）；容器、AEP、顯示與面板型別在此模型沒有物件。
//   屬性來源與 CM_Get_DevNode_PropertyW／CM_Get_Device_Interface_PropertyW 相同（device_property／interface_properties）。
// - 要求的屬性依序輸出，物件沒有的屬性為 DEVPROP_TYPE_EMPTY；DevQueryFlagAllProperties 輸出物件全部屬性（此時不可另列屬性）。
// - 過濾運算式（devfiltertypes.h）：EXISTS、EQUALS、大小比較、BITWISE_AND／OR、BEGINS_WITH／ENDS_WITH／CONTAINS、LIST_*、ARRAY_CONTAINS，NOT 與 IGNORE_CASE 修飾，
//   AND／OR／NOT 群組（最外層為 AND）；EQUALS 搭配 DEVPROP_TYPE_EMPTY 表示屬性不存在。
// - 結果放在程序堆積的單一區塊（DEV_OBJECT 陣列、DEVPROPERTY 陣列、字串與值），由 DevFreeObjects／DevFreeObjectProperties 釋放。
if(name.starts_with("DevGetObjects") || name.starts_with("DevGetObjectProperties") || name.starts_with("DevCreateObjectQuery")){
    constexpr std::uint32_t invalid_argument=0x80070057U,out_of_memory=0x8007000eU,not_found=0x80070490U;
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const unsigned compkey_size=x86_?28U:32U,property_size=x86_?40U:48U,object_size=x86_?16U:32U,filter_size=x86_?44U:56U,parameter_size=x86_?32U:40U;
    const unsigned type_at=x86_?28U:32U,size_at=type_at+4,buffer_at=type_at+8;
    const bool create=name.starts_with("DevCreateObjectQuery"),objects=create || name.starts_with("DevGetObjects"),extended=name.ends_with("Ex");
    struct Requested {DevicePropertyKey key;std::uint32_t store{};};
    struct Value {DevicePropertyKey key;std::uint32_t store{};UnifiedDeviceProperty value;};
    struct Object {std::uint32_t type{};std::u16string id;std::map<DevicePropertyKey,UnifiedDeviceProperty> properties;};
    struct Filter {std::uint32_t op{};DevicePropertyKey key;std::uint32_t store{};UnifiedDeviceProperty value;};
    // 參數位置：DevGetObjects(type, flags, cReq, pReq, cFilter, pFilter, [cExt, pExt,] pcCount, ppObjects)；
    // DevGetObjectProperties(type, pszObjectId, flags, cReq, pReq, [cExt, pExt,] pcCount, ppProperties)。
    const unsigned base=objects?1U:2U;const auto object_type=static_cast<std::uint32_t>(arg(0));
    const auto flags=static_cast<std::uint32_t>(arg(base));const auto requested_count=static_cast<std::uint32_t>(arg(base+1));const auto requested_at=arg(base+2);
    const auto filter_count=objects?static_cast<std::uint32_t>(arg(4)):0U;const auto filter_pointer=objects?arg(5):0;
    const auto parameter_count=extended?static_cast<std::uint32_t>(arg(objects?6:5)):0U;const auto parameter_at=extended?arg(objects?7:6):0;
    // DevCreateObjectQuery(type, flags, cReq, pReq, cFilter, pFilter, [cExt, pExt,] pCallback, pContext, phDevQuery)。
    const auto count_out=create?0:arg(objects?(extended?8:6):(extended?7:5)),result_out=create?0:arg(objects?(extended?9:7):(extended?8:6));
    const auto callback=create?arg(extended?8:6):0,context=create?arg(extended?9:7):0,handle_out=create?arg(extended?10:8):0;
    if(object_type==0 || object_type>12)return finish(invalid_argument);
    if(flags&~0xfU || (!create && (flags&9U)))return finish(invalid_argument); // 同步查詢不接受 DevQueryFlagUpdateResults／DevQueryFlagAsyncClose
    if(!!requested_count!=!!requested_at || (objects && !!filter_count!=!!filter_pointer) || !!parameter_count!=!!parameter_at || (create?(!callback || !handle_out):(!count_out || !result_out)))return finish(invalid_argument);
    if((flags&2U) && requested_count)return finish(invalid_argument);
    if(requested_count>4096 || filter_count>4096 || parameter_count>256)return finish(invalid_argument);
    if(!objects && !arg(1))return finish(invalid_argument);
    // 延伸參數（DEV_QUERY_PARAMETER）只影響 AEP 查詢；此模型沒有 AEP 物件，只檢查可讀。
    if(parameter_count)memory.check(GuestAddress{parameter_at},static_cast<std::size_t>(parameter_count)*parameter_size,Access::Read);
    std::vector<Requested> requested;
    for(std::uint32_t i=0;i<requested_count;++i){
        const auto at=requested_at+static_cast<std::uint64_t>(i)*compkey_size;memory.check(GuestAddress{at},compkey_size,Access::Read);
        Requested item{read_device_property_key(memory,at),static_cast<std::uint32_t>(memory.load(GuestAddress{at+20},32))};
        if(item.store>1)return finish(invalid_argument);requested.push_back(item);
    }
    std::vector<Filter> filters;
    for(std::uint32_t i=0;i<filter_count;++i){
        const auto at=filter_pointer+static_cast<std::uint64_t>(i)*filter_size;memory.check(GuestAddress{at},filter_size,Access::Read);
        const auto property=at+(x86_?4U:8U);Filter item;item.op=static_cast<std::uint32_t>(memory.load(GuestAddress{at},32));
        item.key=read_device_property_key(memory,property);item.store=static_cast<std::uint32_t>(memory.load(GuestAddress{property+20},32));
        item.value.type=static_cast<std::uint32_t>(memory.load(GuestAddress{property+type_at},32));
        const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{property+size_at},32));const auto buffer=memory.load(GuestAddress{property+buffer_at},bits);
        if(size>65536 || (size && !buffer))return finish(invalid_argument);
        item.value.bytes.resize(size);if(size)memory.read(GuestAddress{buffer},item.value.bytes);filters.push_back(std::move(item));
    }
    // 結構檢查：邏輯群組成對、葉節點只用已定義的比較／清單／陣列運算子與修飾。
    {
        std::vector<std::uint32_t> open;
        for(const auto& item:filters){
            const auto logical=item.op&0x0ff00000U;
            if(logical){
                if(item.op&~0x0ff00000U)return finish(invalid_argument);
                if(logical==0x100000U || logical==0x300000U || logical==0x500000U)open.push_back(logical);
                else if(logical==0x200000U || logical==0x400000U || logical==0x600000U){if(open.empty() || open.back()+0x100000U!=logical)return finish(invalid_argument);open.pop_back();}
                else return finish(invalid_argument);
                continue;
            }
            const auto eval=item.op&0xfffU,list=item.op&0xf000U,array=item.op&0xf0000000U,modifier=item.op&0xf0000U;
            if(modifier&~0x30000U)return finish(invalid_argument);
            const int kinds=(eval?1:0)+(list?1:0)+(array?1:0);
            if(kinds!=1 || eval>11 || (list && list>0x4000U) || (array && array!=0x10000000U))return finish(invalid_argument);
            if(!valid_device_property_key(item.key) || item.store>1)return finish(invalid_argument);
        }
        if(!open.empty())return finish(invalid_argument);
    }
    std::uint32_t registry_error=0;
    const auto upper_guid=[&](const std::array<std::uint8_t,16>& guid){auto text=guid_name(guid);for(auto& c:text)if(c>=u'a' && c<=u'z')c=static_cast<char16_t>(c-32);return text;};
    const auto collect=[&]()->std::vector<Object>{
        std::vector<Object> out;const auto empty_guid=[](const std::array<std::uint8_t,16>& guid){return std::all_of(guid.begin(),guid.end(),[](auto byte){return byte==0;});};
        if(object_type==3){
            for(const auto& [handle,node]:nodes){(void)handle;if(node.pending_set)continue;Object item{3,node.id,{}};
                for(const auto& key:device_property_keys(node,nodes))if(auto value=device_property(node,key,nodes))item.properties.emplace(key,std::move(*value));
                out.push_back(std::move(item));}
        }else if(object_type==1){
            RegistryTransaction view(runtime_.registry,registry_file_,&registry_store_cache_,true);if(view.error){registry_error=view.error;return {};}
            for(const auto& [handle,node]:nodes){(void)handle;if(node.pending_set || node.interface_path.empty())continue;
                auto values=interface_properties(node,node.interface_guid,runtime_.registry,registry_error);if(registry_error)return {};
                out.push_back({1,node.interface_path,{values.begin(),values.end()}});}
        }else if(object_type==4 || object_type==7){
            std::set<std::array<std::uint8_t,16>> seen;
            for(const auto& [handle,node]:nodes){(void)handle;if(node.pending_set || (object_type==4 && node.interface_path.empty()))continue;
                const auto& guid=object_type==4?node.interface_guid:node.guid;if(empty_guid(guid) || !seen.insert(guid).second)continue;
                out.push_back({object_type,upper_guid(guid),{}});}
        }
        return out;
    };
    // 過濾運算式求值
    const auto text_of=[](const std::vector<std::uint8_t>& bytes){std::u16string text;for(std::size_t i=0;i+1<bytes.size();i+=2){const auto c=static_cast<char16_t>(bytes[i]|(std::uint16_t{bytes[i+1]}<<8));if(!c)break;text.push_back(c);}return text;};
    const auto list_of=[](const std::vector<std::uint8_t>& bytes){std::vector<std::u16string> items;std::u16string current;
        for(std::size_t i=0;i+1<bytes.size();i+=2){const auto c=static_cast<char16_t>(bytes[i]|(std::uint16_t{bytes[i+1]}<<8));if(c)current.push_back(c);else{if(current.empty())break;items.push_back(std::move(current));current.clear();}}
        if(!current.empty())items.push_back(std::move(current));return items;};
    const auto fold_case=[](std::u16string text){for(auto& c:text)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);return text;};
    const auto element_size=[](std::uint32_t type)->std::size_t{
        switch(type&0xfffU){case 2:case 3:case 17:return 1;case 4:case 5:return 2;case 6:case 7:case 10:case 22:case 23:case 24:return 4;case 8:case 9:case 11:case 14:case 15:case 16:return 8;case 13:return 16;case 21:return 20;default:return 0;}};
    const auto integer=[](std::uint32_t type,const std::uint8_t* data,bool& is_signed)->std::uint64_t{
        std::uint64_t value=0;const auto width=type==2 || type==3 || type==17?1U:type==4 || type==5?2U:type==6 || type==7 || type==22 || type==23 || type==24?4U:8U;
        for(unsigned i=0;i<width;++i)value|=std::uint64_t{data[i]}<<(8*i);is_signed=type==2 || type==4 || type==6 || type==8;
        if(is_signed && width<8 && (value>>(width*8-1)&1U))value|=~std::uint64_t{0}<<(width*8);return value;};
    const auto numeric=[](std::uint32_t type){return (type>=2 && type<=9) || type==16 || type==17 || type==22 || type==23 || type==24;};
    const auto compare_values=[&](const UnifiedDeviceProperty& left,const UnifiedDeviceProperty& right,bool ignore_case)->std::optional<int>{
        // 同型別才可比較：整數依有號／無號、浮點、字串依序數（可忽略大小寫），其他型別只判斷相等。
        if(left.type!=right.type)return std::nullopt;const auto type=left.type;
        if(numeric(type)){const auto width=element_size(type);if(left.bytes.size()<width || right.bytes.size()<width)return std::nullopt;bool sign=false;
            const auto a=integer(type,left.bytes.data(),sign),b=integer(type,right.bytes.data(),sign);
            if(sign){const auto x=static_cast<std::int64_t>(a),y=static_cast<std::int64_t>(b);return x<y?-1:x>y?1:0;}return a<b?-1:a>b?1:0;}
        if(type==10 || type==11){if(left.bytes.size()<element_size(type) || right.bytes.size()<element_size(type))return std::nullopt;double a=0,b=0;
            if(type==10){float x,y;std::memcpy(&x,left.bytes.data(),4);std::memcpy(&y,right.bytes.data(),4);a=x;b=y;}else{std::memcpy(&a,left.bytes.data(),8);std::memcpy(&b,right.bytes.data(),8);}
            return a<b?-1:a>b?1:0;}
        if(type==18 || type==25 || type==20){auto a=text_of(left.bytes),b=text_of(right.bytes);if(ignore_case){a=fold_case(a);b=fold_case(b);}return a<b?-1:a>b?1:0;}
        if(type==0x2012U || type==0x2019U){auto a=list_of(left.bytes),b=list_of(right.bytes);if(ignore_case){for(auto& x:a)x=fold_case(x);for(auto& x:b)x=fold_case(x);}return a==b?0:a<b?-1:1;}
        return left.bytes==right.bytes?0:left.bytes<right.bytes?-1:1;
    };
    const auto leaf=[&](const Filter& item,const Object& object)->bool{
        const auto found=item.store==0?object.properties.find(item.key):object.properties.end();const bool present=found!=object.properties.end() && found->second.type;
        const auto eval=item.op&0xfffU,list=item.op&0xf000U;const bool ignore_case=(item.op&0x20000U)!=0,negate=(item.op&0x10000U)!=0;bool result=false;
        if(eval==1)result=present;
        else if(eval==2 && !item.value.type)result=!present;
        else if(present){
            const auto& value=found->second;
            if(eval==2){const auto order=compare_values(value,item.value,ignore_case);result=order && *order==0;}
            else if(eval>=3 && eval<=6){const auto order=compare_values(value,item.value,ignore_case);
                result=order && (eval==3?*order>0:eval==4?*order<0:eval==5?*order>=0:*order<=0);}
            else if(eval==7 || eval==8){
                if(value.type==item.value.type && numeric(value.type) && value.bytes.size()>=element_size(value.type) && item.value.bytes.size()>=element_size(value.type)){bool sign=false;
                    const auto a=integer(value.type,value.bytes.data(),sign),b=integer(value.type,item.value.bytes.data(),sign);result=eval==7?(a&b)==b:(a&b)!=0;}
            }else if(eval>=9 && eval<=11){
                if((value.type==18 || value.type==25) && item.value.type==value.type){auto a=text_of(value.bytes),b=text_of(item.value.bytes);if(ignore_case){a=fold_case(a);b=fold_case(b);}
                    result=eval==9?a.starts_with(b):eval==10?a.ends_with(b):a.find(b)!=a.npos;}
            }else if(list){
                if((value.type&0x2000U) && (item.value.type==(value.type&0xfffU))){auto b=text_of(item.value.bytes);if(ignore_case)b=fold_case(b);
                    for(auto element:list_of(value.bytes)){if(ignore_case)element=fold_case(element);
                        if(list==0x1000U?element==b:list==0x2000U?element.starts_with(b):list==0x3000U?element.ends_with(b):element.find(b)!=element.npos){result=true;break;}}}
            }else if(item.op&0x10000000U){
                const auto width=element_size(value.type);
                if((value.type&0x1000U) && item.value.type==(value.type&0xfffU) && width && item.value.bytes.size()>=width)
                    for(std::size_t at=0;at+width<=value.bytes.size();at+=width)if(std::equal(item.value.bytes.begin(),item.value.bytes.begin()+static_cast<std::ptrdiff_t>(width),value.bytes.begin()+static_cast<std::ptrdiff_t>(at))){result=true;break;}
            }
        }
        return negate?!result:result;
    };
    // 群組求值：mode 為 AND（最外層與 AND_OPEN）、OR、NOT（內容以 AND 結合後取反）；at 停在對應的關閉運算子之後。
    const std::function<bool(std::size_t&,std::uint32_t,const Object&)> group=[&](std::size_t& at,std::uint32_t mode,const Object& object)->bool{
        bool all=true,any=false;
        while(at<filters.size()){
            const auto logical=filters[at].op&0x0ff00000U;
            if(logical==0x200000U || logical==0x400000U || logical==0x600000U){++at;break;}
            bool value;
            if(logical){++at;value=group(at,logical,object);}else value=leaf(filters[at++],object);
            all=all && value;any=any || value;
        }
        return mode==0x300000U?any:mode==0x500000U?!all:all;
    };
    const auto matches=[&](const Object& object){std::size_t at=0;return filters.empty() || group(at,0,object);};
    const auto select=[&](const Object& object){
        std::vector<Value> out;
        if(flags&2U){for(const auto& [key,value]:object.properties)out.push_back({key,0,value});}
        else for(const auto& item:requested){Value value{item.key,item.store,{}};if(!item.store)if(const auto found=object.properties.find(item.key);found!=object.properties.end())value.value=found->second;out.push_back(std::move(value));}
        return out;
    };
    // 輸出區塊：[DEV_OBJECT × n][DEVPROPERTY × 全部][識別碼與值]；DevGetObjectProperties 只有 DEVPROPERTY 陣列與值。
    const auto align=[](std::uint64_t value){return (value+7)&~std::uint64_t{7};};
    const auto emit=[&](const std::vector<std::pair<const Object*,std::vector<Value>>>& items)->std::pair<std::uint32_t,std::uint64_t>{
        std::uint64_t properties=0,data=0;
        for(const auto& [object,values]:items){if(objects)data+=align((object->id.size()+1)*2);properties+=values.size();for(const auto& value:values)data+=align(value.value.bytes.size());}
        const auto header=objects?align(static_cast<std::uint64_t>(items.size())*object_size):0;const auto table=align(properties*property_size);
        const auto total=header+table+data;if(!total)return {0,0};if(total>output_limit)return {out_of_memory,0};
        const auto block=allocate_heap(memory,total);if(!block)return {out_of_memory,0};
        memory.write(GuestAddress{block},std::vector<std::uint8_t>(total));
        auto property_at=block+header,data_at=block+header+table;
        for(std::size_t index=0;index<items.size();++index){
            const auto& [object,values]=items[index];const auto first=property_at;
            for(const auto& value:values){
                memory.write(GuestAddress{property_at},value.key.format);memory.store(GuestAddress{property_at+16},32,value.key.id);memory.store(GuestAddress{property_at+20},32,value.store);
                memory.store(GuestAddress{property_at+type_at},32,value.value.type);memory.store(GuestAddress{property_at+size_at},32,value.value.bytes.size());
                if(!value.value.bytes.empty()){memory.write(GuestAddress{data_at},value.value.bytes);memory.store(GuestAddress{property_at+buffer_at},bits,data_at);data_at+=align(value.value.bytes.size());}
                property_at+=property_size;
            }
            if(objects){
                const auto at=block+index*object_size;memory.store(GuestAddress{at},32,object->type);
                memory.write(GuestAddress{data_at},utf16_bytes(object->id));memory.store(GuestAddress{at+unit},bits,data_at);data_at+=align((object->id.size()+1)*2);
                memory.store(GuestAddress{at+unit*2},32,values.size());memory.store(GuestAddress{at+unit*3},bits,values.empty()?0:first);
            }
        }
        device_configuration_->query_blocks.insert(block);return {0,block};
    };
    const auto all=collect();if(registry_error)return finish(0x80070000U|registry_error);
    std::vector<std::pair<const Object*,std::vector<Value>>> items;
    if(objects){for(const auto& object:all)if(matches(object))items.emplace_back(&object,select(object));}
    else{
        const auto id=fold_case(read_utf16(memory,GuestAddress{arg(1)},32768));
        const auto found=std::find_if(all.begin(),all.end(),[&](const auto& object){return fold_case(object.id)==id;});if(found==all.end())return finish(not_found);
        items.emplace_back(&*found,select(*found));
    }
    if(create){
        // 非同步查詢：建立時取得結果（與 DevGetObjects 相同），回呼在執行緒池依序送出；結果區塊屬於查詢，DevFreeObjects 不可釋放。
        auto& queries=device_configuration_->queries;if(queries.size()>=1024)return finish(out_of_memory);
        memory.check(GuestAddress{handle_out},unit,Access::Write);
        const auto [error,block]=emit(items);if(error)return finish(error);if(block)device_configuration_->query_blocks.erase(block);
        const auto handle=device_configuration_->next_query++;
        DeviceConfigurationState::Query query;query.callback=callback;query.context=context;query.block=block;query.count=block?static_cast<std::uint32_t>(items.size()):0U;query.flags=flags;query.running=true;
        queries.emplace(handle,query);memory.store(GuestAddress{handle_out},bits,handle);
        if(const auto queued=queue_pool_callback(state,memory,{resolve("internal-devquery","Deliver").value,{handle}})){
            if(block)(void)free_heap(memory,block,state);queries.erase(handle);memory.store(GuestAddress{handle_out},bits,0);return finish(0x80070000U|queued);
        }
        return finish(0);
    }
    memory.check(GuestAddress{count_out},4,Access::Write);memory.check(GuestAddress{result_out},unit,Access::Write);
    const auto [error,block]=emit(items);if(error)return finish(error);
    std::uint64_t count=objects?items.size():items.front().second.size();if(!block)count=0;
    memory.store(GuestAddress{count_out},32,count);memory.store(GuestAddress{result_out},bits,block);return finish(0);
}
