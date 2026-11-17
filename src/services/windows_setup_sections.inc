        // SetupAPI 的 INF 安裝區段：SetupInstallFromInfSection、SetupInstallServicesFromInfSection 與 InstallHinfSection（rundll32 入口）。
        // 先在宿主端把 INF 指示轉成操作清單（含 Include／Needs），再以協程依序套用：檔案（建立佇列後經 SetupCommitFileQueue 提交）、UnregisterDlls、
        // DelReg、AddReg、UpdateInis、UpdateIniFields、Ini2Reg、BitReg、RegisterDlls、CopyINF；登錄、INI、服務與 DLL 註冊都呼叫既有的客體入口。
        if(name.starts_with("SetupInstallFromInfSection") || name.starts_with("SetupInstallServicesFromInfSection") || name.starts_with("InstallHinfSection")){
            const bool unicode=name.ends_with('W');
            const auto u32=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
            const auto hkey=[&](std::uint32_t index){return x86_?std::uint64_t{0x80000000U+index}:0xffffffff80000000ULL+index;};
            // 登錄操作：kind 0 新增、1 刪除、2 位元；relative 為 HKR（套用時換成呼叫端或服務的機碼）。
            struct RegOp {std::uint32_t kind{};std::uint64_t root{};bool relative{},has_value{},key_only{};std::u16string subkey,value;std::uint32_t flags{},type{};
                std::vector<std::uint8_t> data;std::vector<std::u16string> strings;std::uint32_t mask{},index{};};
            const auto root_of=[&](const std::u16string& text,RegOp& op){
                const auto key=fold(text);
                if(key==u"hkr"){op.relative=true;return;}
                if(key==u"hkcr" || key==u"hkey_classes_root")op.root=hkey(0);else if(key==u"hkcu" || key==u"hkey_current_user")op.root=hkey(1);
                else if(key==u"hklm" || key==u"hkey_local_machine")op.root=hkey(2);else if(key==u"hku" || key==u"hkey_users")op.root=hkey(3);else throw Failure{13};
            };
            const auto number_of=[](const std::u16string& text)->std::optional<std::uint64_t>{if(text.empty())return std::nullopt;return driver_store::number(text);};
            // 二進位欄位為十六進位位元組（可帶 0x）。
            const auto hex_bytes=[&](const std::vector<std::u16string>& values){
                std::vector<std::uint8_t> bytes;
                for(const auto& value:values){if(value.empty())continue;const auto parsed=driver_store::number(value.starts_with(u"0x") || value.starts_with(u"0X")?value:u"0x"+value);
                    if(!parsed || *parsed>255)throw Failure{13};bytes.push_back(static_cast<std::uint8_t>(*parsed));}
                return bytes;};
            const auto multi_bytes=[](const std::vector<std::u16string>& items){std::u16string text;for(const auto& item:items){text+=item;text.push_back(0);}text.push_back(0);auto bytes=utf16_bytes(text);bytes.resize(text.size()*2);return bytes;};
            const auto parse_registry=[&](State::Handle& handle,const std::u16string& section,std::uint32_t kind,std::vector<RegOp>& ops){
                for(const auto& row:section_rows(handle,section)){
                    RegOp op;op.kind=kind;root_of(row[0],op);op.subkey=row.size()>1?row[1]:std::u16string{};op.has_value=row.size()>2;op.value=op.has_value?row[2]:std::u16string{};
                    if(row.size()>3 && !row[3].empty()){const auto flags=number_of(row[3]);if(!flags)throw Failure{13};op.flags=static_cast<std::uint32_t>(*flags);}
                    const std::vector<std::u16string> values(row.begin()+static_cast<std::ptrdiff_t>(std::min<std::size_t>(4,row.size())),row.end());
                    if(kind==0 && (op.flags&0x8004U))op.kind=1; // AddReg 行帶 FLG_ADDREG_DELREG_BIT／DELVAL 時為刪除
                    if(op.kind==0){
                        if(!op.has_value || (op.flags&0x2010U)){op.key_only=true;}
                        else switch(op.flags&0xffff0001U){
                        case 0x00000000U:op.type=1;op.data=utf16_bytes(values.empty()?std::u16string{}:values[0]);break;
                        case 0x00020000U:op.type=2;op.data=utf16_bytes(values.empty()?std::u16string{}:values[0]);break;
                        case 0x00010000U:op.type=7;op.strings=values;std::erase_if(op.strings,[](const std::u16string& item){return item.empty();});op.data=multi_bytes(op.strings);break;
                        case 0x00000001U:op.type=3;op.data=hex_bytes(values);break;
                        case 0x00010001U:op.type=4;
                            if(values.size()<=1){const auto value=values.empty()?std::optional<std::uint64_t>{0}:number_of(values[0]);if(!value)throw Failure{13};
                                for(unsigned i=0;i<4;++i)op.data.push_back(static_cast<std::uint8_t>(*value>>(8*i)));}
                            else op.data=hex_bytes(values);break;
                        case 0x00020001U:op.type=0;op.data=hex_bytes(values);break;
                        default:op.type=op.flags>>16;op.data=hex_bytes(values);break;
                        }
                    }else if(op.kind==1){if(!values.empty())op.strings.push_back(values[0]);}
                    else{const auto mask=values.size()>0?number_of(values[0]):std::nullopt;const auto index=values.size()>1?number_of(values[1]):std::nullopt;
                        if(!mask || !index || *mask>255)throw Failure{13};op.mask=static_cast<std::uint32_t>(*mask);op.index=static_cast<std::uint32_t>(*index);}
                    ops.push_back(std::move(op));
                }
            };
            // 套用登錄操作（只捕獲 this，可在各協程共用）：找不到的機碼與值在刪除時忽略。
            const auto apply_registry=[this,multi_bytes](GuestState& s,GuestMemory& m,std::vector<RegOp> ops,std::uint64_t relative)->service_callback::Task<std::uint32_t>{
                const unsigned size=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(size*8);std::vector<std::uint64_t> blocks;
                const auto release=[&]{for(const auto block:blocks)(void)free_heap(m,block,s);blocks.clear();};
                const auto scratch=[&](std::size_t length)->std::uint64_t{const auto at=allocate_heap(m,std::max<std::size_t>(length,1));if(!at)throw std::bad_alloc{};blocks.push_back(at);m.fill(GuestAddress{at},0,std::max<std::size_t>(length,1));return at;};
                const auto text=[&](const std::u16string& value){const auto bytes=utf16_bytes(value);const auto at=scratch(bytes.size());m.write(GuestAddress{at},bytes);return at;};
                const auto api=[&](std::string_view symbol,std::vector<std::uint64_t> arguments)->service_callback::Task<std::uint32_t>{
                    co_return static_cast<std::uint32_t>(co_await service_callback::call_value(resolve("advapi32.dll",symbol).value,std::move(arguments)));};
                // 讀出 REG_MULTI_SZ（不存在時為空）。
                const auto read_multi=[&](std::uint64_t key,std::uint64_t value_at)->service_callback::Task<std::vector<std::u16string>>{
                    std::vector<std::u16string> items;const auto length_at=scratch(4);m.store(GuestAddress{length_at},32,0);
                    if(co_await api("RegQueryValueExW",{key,value_at,0,0,0,length_at}))co_return items;
                    const auto length=static_cast<std::uint32_t>(m.load(GuestAddress{length_at},32));if(!length || length>1048576)co_return items;
                    const auto data=scratch(length+4);if(co_await api("RegQueryValueExW",{key,value_at,0,0,data,length_at}))co_return items;
                    std::u16string joined;for(std::uint32_t i=0;i+1<length;i+=2)joined.push_back(static_cast<char16_t>(m.load(GuestAddress{data+i},16)));
                    for(std::size_t begin=0;begin<joined.size();){const auto end=joined.find(u'\0',begin);const auto item=joined.substr(begin,end==std::u16string::npos?std::u16string::npos:end-begin);
                        if(!item.empty())items.push_back(item);if(end==std::u16string::npos)break;begin=end+1;}
                    co_return items;};
                std::uint32_t failure=0;
                for(const auto& op:ops){
                    const auto root=op.relative?relative:op.root;if(!root){failure=87;break;}
                    const std::uint32_t access=0xf003fU|((op.flags&0x1000U)?0x100U:0U)|((op.flags&0x4000U)?0x200U:0U);
                    const auto key_at=scratch(size);const auto subkey=text(op.subkey),value_at=text(op.value);
                    if(op.kind==1 && !op.has_value){
                        // 沒有值名稱：刪除整個子機碼（含子機碼與值）。
                        if(!op.subkey.empty()){(void)co_await api("RegDeleteTreeW",{root,subkey});(void)co_await api("RegDeleteKeyExW",{root,subkey,access&0x300U,0});}
                        release();continue;
                    }
                    std::uint32_t error=op.kind==0?co_await api("RegCreateKeyExW",{root,subkey,0,0,0,access,0,key_at,0}):co_await api("RegOpenKeyExW",{root,subkey,0,access,key_at});
                    if(error){release();if(op.kind==1 && (error==2 || error==3))continue;failure=error;break;}
                    const auto key=m.load(GuestAddress{key_at},bits);
                    if(op.kind==0 && !op.key_only){
                        const auto length_at=scratch(4);m.store(GuestAddress{length_at},32,0);const bool present=!(co_await api("RegQueryValueExW",{key,value_at,0,0,0,length_at}));
                        bool write=true;std::vector<std::uint8_t> data=op.data;
                        if((op.flags&0x2U) && present)write=false;                   // FLG_ADDREG_NOCLOBBER
                        if((op.flags&0x20U) && !present)write=false;                  // FLG_ADDREG_OVERWRITEONLY
                        if(write && (op.flags&0x8U) && op.type==7){                   // FLG_ADDREG_APPEND：只加入尚未存在的字串
                            auto items=co_await read_multi(key,value_at);
                            for(const auto& item:op.strings)if(std::none_of(items.begin(),items.end(),[&](const std::u16string& existing){return fold(existing)==fold(item);}))items.push_back(item);
                            data=multi_bytes(items);
                        }
                        if(write){const auto data_at=scratch(data.size());m.write(GuestAddress{data_at},data);error=co_await api("RegSetValueExW",{key,value_at,0,op.type,data_at,data.size()});}
                    }else if(op.kind==1){
                        if(!op.strings.empty() && (op.flags&0xffff80ffU)==0x00018002U){ // FLG_DELREG_MULTI_SZ_DELSTRING
                            auto items=co_await read_multi(key,value_at);std::erase_if(items,[&](const std::u16string& item){return fold(item)==fold(op.strings.front());});
                            const auto data=multi_bytes(items);const auto data_at=scratch(data.size());m.write(GuestAddress{data_at},data);error=co_await api("RegSetValueExW",{key,value_at,0,7,data_at,data.size()});
                        }else{error=co_await api("RegDeleteValueW",{key,value_at});if(error==2)error=0;}
                    }else if(op.kind==2){
                        // BitReg：既有二進位值的指定位元組設定或清除位元。
                        const auto type_at=scratch(4),length_at=scratch(4);const auto data_at=scratch(4096);m.store(GuestAddress{length_at},32,4096);
                        error=co_await api("RegQueryValueExW",{key,value_at,0,type_at,data_at,length_at});
                        if(!error){const auto length=static_cast<std::uint32_t>(m.load(GuestAddress{length_at},32));
                            if(op.index>=length)error=13;
                            else{auto byte=static_cast<std::uint8_t>(m.load(GuestAddress{data_at+op.index},8));byte=(op.flags&1U)?static_cast<std::uint8_t>(byte|op.mask):static_cast<std::uint8_t>(byte&~op.mask);
                                m.store(GuestAddress{data_at+op.index},8,byte);error=co_await api("RegSetValueExW",{key,value_at,0,m.load(GuestAddress{type_at},32),data_at,length});}}
                    }
                    (void)co_await api("RegCloseKey",{key});release();
                    if(error){failure=error;break;}
                }
                release();co_return failure;
            };
            // INI 操作：kind 0 UpdateInis、1 UpdateIniFields、2 Ini2Reg。
            struct IniOp {std::uint32_t kind{},flags{};std::u16string file,section,key,old_value,new_key,new_value,old_field,new_field;RegOp target;};
            const auto split_entry=[](const std::u16string& entry,std::u16string& key,std::u16string& value){const auto at=entry.find(u'=');key=entry.substr(0,at);value=at==std::u16string::npos?std::u16string{}:entry.substr(at+1);
                const auto trim=[](std::u16string& text){while(!text.empty() && text.back()==u' ')text.pop_back();while(!text.empty() && text.front()==u' ')text.erase(0,1);};trim(key);trim(value);};
            const auto parse_ini=[&](State::Handle& handle,const std::u16string& section,std::uint32_t kind,std::vector<IniOp>& ops){
                for(const auto& row:section_rows(handle,section)){
                    IniOp op;op.kind=kind;op.file=row[0];op.section=row.size()>1?row[1]:std::u16string{};if(op.file.empty() || op.section.empty())throw Failure{13};
                    if(kind==0){
                        // 檔名, 區段, [舊項目 key=value], [新項目 key=value], [旗標]：旗標 1 時值可用 * 萬用字元比對。
                        if(row.size()>2)split_entry(row[2],op.key,op.old_value);if(row.size()>3)split_entry(row[3],op.new_key,op.new_value);
                        if(row.size()>4 && !row[4].empty()){const auto flags=number_of(row[4]);if(!flags)throw Failure{13};op.flags=static_cast<std::uint32_t>(*flags);}
                    }else if(kind==1){
                        // 檔名, 區段, 項目名稱, [舊欄位], [新欄位], [旗標]：旗標 2 以逗號分隔欄位，否則以空白分隔。
                        op.key=row.size()>2?row[2]:std::u16string{};if(op.key.empty())throw Failure{13};op.old_field=row.size()>3?row[3]:std::u16string{};op.new_field=row.size()>4?row[4]:std::u16string{};
                        if(row.size()>5 && !row[5].empty()){const auto flags=number_of(row[5]);if(!flags)throw Failure{13};op.flags=static_cast<std::uint32_t>(*flags);}
                    }else{
                        // 檔名, 區段, [項目名稱], 登錄根, 子機碼[, 旗標]：把 INI 項目複製成 REG_SZ。
                        op.key=row.size()>2?row[2]:std::u16string{};if(row.size()<5)throw Failure{13};root_of(row[3],op.target);op.target.subkey=row[4];
                        if(row.size()>5 && !row[5].empty()){const auto flags=number_of(row[5]);if(!flags)throw Failure{13};op.target.flags=static_cast<std::uint32_t>(*flags);}
                    }
                    ops.push_back(std::move(op));
                }
            };
            struct DllOp {std::u16string path,argument;std::uint32_t flags{};bool unregister{};};
            // 安裝區段的指示（含 Needs 引用的區段，先處理被引用者；Include 的 INF 附加到同一 handle）。
            struct Plan {std::vector<RegOp> delreg,addreg,bitreg;std::vector<IniOp> inis;std::vector<DllOp> dlls;std::vector<std::u16string> copy_infs,sections;};
            const auto build_plan=[&](State::Handle& handle,const std::u16string& section,std::uint32_t flags,Plan& plan){
                std::set<std::u16string> visited;
                const auto visit=[&](const auto& self,const std::u16string& current)->void{
                    if(!visited.insert(fold(current)).second)return;
                    const auto rows=section_rows(handle,current);
                    for(const auto& row:rows){
                        const auto key=fold(row[0]);
                        if(key==u"include")for(std::size_t i=1;i<row.size();++i){
                            if(row[i].empty())continue;const auto path=row[i].find_first_of(u"\\/:")==std::u16string::npos && exists(driver_store::parent_of(handle.files.front().path)+u"\\"+row[i])?driver_store::parent_of(handle.files.front().path)+u"\\"+row[i]:locate(row[i]);
                            if(std::none_of(handle.files.begin(),handle.files.end(),[&](const State::File& file){return fold(file.path)==fold(path);}))handle.files.push_back(load(path,nullptr));}
                    }
                    for(const auto& row:rows)if(fold(row[0])==u"needs")for(std::size_t i=1;i<row.size();++i)if(!row[i].empty())self(self,row[i]);
                    plan.sections.push_back(current);
                    for(const auto& row:rows){
                        const auto key=fold(row[0]);
                        for(std::size_t i=1;i<row.size();++i){
                            const auto& value=row[i];if(value.empty())continue;
                            if(key==u"delreg" && (flags&4U))parse_registry(handle,value,1,plan.delreg);
                            else if(key==u"addreg" && (flags&4U))parse_registry(handle,value,0,plan.addreg);
                            else if(key==u"bitreg" && (flags&0x20U))parse_registry(handle,value,2,plan.bitreg);
                            else if(key==u"updateinis" && (flags&2U))parse_ini(handle,value,0,plan.inis);
                            else if(key==u"updateinifields" && (flags&2U))parse_ini(handle,value,1,plan.inis);
                            else if(key==u"ini2reg" && (flags&8U))parse_ini(handle,value,2,plan.inis);
                            else if((key==u"registerdlls" && (flags&0x40U)) || (key==u"unregisterdlls" && (flags&0x80U))){
                                // dirid, 子目錄, 檔名, 註冊旗標（1 DllRegisterServer、2 DllInstall）, [逾時], [DllInstall 參數]
                                for(const auto& line:section_rows(handle,value)){
                                    if(line.size()<4)throw Failure{13};const auto id=number_of(line[0]);const auto mode=number_of(line[3]);if(!id || !mode)throw Failure{13};
                                    const auto base=handle_dirid(handle,static_cast<std::int64_t>(static_cast<std::int32_t>(*id)),handle.files.front());if(!base)throw Failure{13};
                                    plan.dlls.push_back({join(join(*base,line[1]),line[2]),line.size()>5?line[5]:std::u16string{},static_cast<std::uint32_t>(*mode),key==u"unregisterdlls"});
                                }
                            }else if(key==u"copyinf" && (flags&0x200U))plan.copy_infs.push_back(value.find_first_of(u"\\/:")==std::u16string::npos?driver_store::parent_of(handle.files.front().path)+u"\\"+value:full_path(value));
                        }
                    }
                };
                visit(visit,section);
            };
            // 套用 INI 操作（協程內，使用 kernel32 的 Profile 入口）。
            const auto apply_ini=[this,apply_registry](GuestState& s,GuestMemory& m,std::vector<IniOp> ops,std::uint64_t relative)->service_callback::Task<std::uint32_t>{
                std::vector<std::uint64_t> blocks;
                const auto release=[&]{for(const auto block:blocks)(void)free_heap(m,block,s);blocks.clear();};
                const auto text=[&](const std::u16string& value)->std::uint64_t{const auto bytes=utf16_bytes(value);const auto at=allocate_heap(m,bytes.size());if(!at)throw std::bad_alloc{};blocks.push_back(at);m.write(GuestAddress{at},bytes);return at;};
                const auto kernel=[&](std::string_view symbol,std::vector<std::uint64_t> arguments)->service_callback::Task<std::uint32_t>{
                    co_return static_cast<std::uint32_t>(co_await service_callback::call_value(resolve("kernel32.dll",symbol).value,std::move(arguments)));};
                const auto read=[&](const IniOp& op,const std::u16string& key)->service_callback::Task<std::optional<std::u16string>>{
                    constexpr std::uint32_t capacity=32768;const auto buffer=allocate_heap(m,capacity*2);if(!buffer)throw std::bad_alloc{};blocks.push_back(buffer);
                    const auto marker=u"\x01\x02";const auto length=co_await kernel("GetPrivateProfileStringW",{text(op.section),key.empty()?0:text(key),text(marker),buffer,capacity,text(op.file)});
                    auto value=read_utf16(m,GuestAddress{buffer},capacity);if(key.empty()){std::u16string all;for(std::uint32_t i=0;i<length;++i)all.push_back(static_cast<char16_t>(m.load(GuestAddress{buffer+i*2},16)));co_return all;}
                    if(value==marker)co_return std::nullopt;co_return value;};
                const auto matches=[](const IniOp& op,const std::u16string& value){if(op.old_value.empty() || op.old_value==u"*")return true;
                    if((op.flags&1U) && op.old_value.ends_with(u'*'))return fold(value).starts_with(fold(op.old_value.substr(0,op.old_value.size()-1)));return fold(value)==fold(op.old_value);};
                for(const auto& op:ops){
                    if(op.kind==0){
                        if(!op.key.empty()){const auto current=co_await read(op,op.key);if(current && matches(op,*current))(void)co_await kernel("WritePrivateProfileStringW",{text(op.section),text(op.key),0,text(op.file)});}
                        if(!op.new_key.empty())(void)co_await kernel("WritePrivateProfileStringW",{text(op.section),text(op.new_key),text(op.new_value),text(op.file)});
                    }else if(op.kind==1){
                        const auto current=co_await read(op,op.key);const char16_t separator=(op.flags&2U)?u',':u' ';std::vector<std::u16string> fields;
                        if(current)for(std::size_t begin=0;begin<=current->size();){const auto end=current->find(separator,begin);auto field=current->substr(begin,end==std::u16string::npos?std::u16string::npos:end-begin);
                            if(!field.empty())fields.push_back(field);if(end==std::u16string::npos)break;begin=end+1;}
                        const auto same=[&](const std::u16string& field){return (op.flags&1U) && op.old_field.ends_with(u'*')?fold(field).starts_with(fold(op.old_field.substr(0,op.old_field.size()-1))):fold(field)==fold(op.old_field);};
                        bool replaced=false;
                        if(!op.old_field.empty())for(auto it=fields.begin();it!=fields.end();){if(same(*it)){if(op.new_field.empty())it=fields.erase(it);else{*it=op.new_field;replaced=true;++it;}}else ++it;}
                        if(!replaced && !op.new_field.empty() && std::none_of(fields.begin(),fields.end(),[&](const std::u16string& field){return fold(field)==fold(op.new_field);}))fields.push_back(op.new_field);
                        std::u16string joined;for(std::size_t i=0;i<fields.size();++i){if(i)joined.push_back(separator);joined+=fields[i];}
                        (void)co_await kernel("WritePrivateProfileStringW",{text(op.section),text(op.key),text(joined),text(op.file)});
                    }else{
                        std::vector<RegOp> writes;std::vector<std::u16string> keys;
                        if(!op.key.empty())keys.push_back(op.key);
                        else if(const auto all=co_await read(op,{}))for(std::size_t begin=0;begin<all->size();){const auto end=all->find(u'\0',begin);const auto item=all->substr(begin,end==std::u16string::npos?std::u16string::npos:end-begin);if(!item.empty())keys.push_back(item);if(end==std::u16string::npos)break;begin=end+1;}
                        for(const auto& key:keys)if(const auto value=co_await read(op,key)){RegOp write=op.target;write.kind=0;write.has_value=true;write.value=key;write.type=1;write.data=utf16_bytes(*value);writes.push_back(std::move(write));}
                        release();if(const auto error=co_await apply_registry(s,m,std::move(writes),relative)){co_return error;}
                    }
                    release();
                }
                co_return 0U;
            };
            if(name.starts_with("SetupInstallFromInfSection")){
                // (Owner, InfHandle, SectionName, Flags, RelativeKeyRoot, SourceRootPath, CopyFlags, MsgHandler, Context, DeviceInfoSet, DeviceInfoData)
                auto& handle=find_handle(arg(1));if(!arg(2))return fail(87);const auto flags=u32(3);
                if(flags&~(0x7ffU|0x10000U|0x20000U|0x40000U|0x80000U|0x100000U))return fail(87);
                if((flags&0x10U) && !arg(7))return fail(87);
                if((flags&0x80000U) && !arg(7))return fail(87);
                Plan plan;build_plan(handle,text_arg(2),flags,plan);
                std::uint64_t queue=0;
                if(flags&0x10U){
                    while(!queue || queues.contains(queue)){queue=allocate_heap(memory,16);if(!queue)return fail(8);}
                    queues.emplace(queue,State::FileQueue{});
                    try{for(const auto& section:plan.sections)queue_install_section(queue,handle,arg(1),handle,arg(1),section,text_arg(5),u32(6));}
                    catch(...){queues.erase(queue);(void)free_heap(memory,queue,state);throw;}
                }
                const auto owner=arg(0),relative=arg(4),handler=arg(7),context=arg(8);const bool aware=flags&0x80000U;
                const auto commit=resolve("setupapi.dll",unicode?"SetupCommitFileQueueW":"SetupCommitFileQueueA").value,close=resolve("setupapi.dll","SetupCloseFileQueue").value;
                const auto copy_inf=resolve("setupapi.dll","SetupCopyOEMInfW").value;
                return nested_continuation(state,memory,"SetupInstallFromInfSection",
                    [this,service,plan,queue,owner,relative,handler,context,aware,commit,close,copy_inf,apply_registry,apply_ini](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                        const WindowsCall inner(s,m,service);std::uint32_t failure=0;bool ok=true;
                        const auto finish_with=[&](bool success,std::uint32_t error){last_error_=success?0:error;inner.return_to(s,success?1:0);};
                        if(queue){
                            ok=co_await service_callback::call(commit,{owner,queue,handler,context})!=0;
                            if(!ok){const auto saved=last_error_;(void)co_await service_callback::call(close,{queue});finish_with(false,saved?saved:1223);co_return std::optional<std::uint32_t>{};}
                            (void)co_await service_callback::call(close,{queue});
                        }
                        const unsigned size=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(size*8);
                        // DLL 註冊：LoadLibraryW → GetProcAddress → DllRegisterServer／DllUnregisterServer 或 DllInstall(BOOL, PCWSTR)；
                        // SPINST_REGISTERCALLBACKAWARE 時以 STARTREGISTRATION／ENDREGISTRATION（SP_REGISTER_CONTROL_STATUS）通知。
                        const auto run_dlls=[&](bool unregister)->service_callback::Task<std::uint32_t>{
                            for(const auto& dll:plan.dlls){
                                if(dll.unregister!=unregister)continue;
                                std::vector<std::uint64_t> blocks;
                                const auto put=[&](const std::u16string& value)->std::uint64_t{const auto bytes=utf16_bytes(value);const auto at=allocate_heap(m,bytes.size());if(!at)throw std::bad_alloc{};blocks.push_back(at);m.write(GuestAddress{at},bytes);return at;};
                                const auto put_ansi=[&](std::string_view value)->std::uint64_t{const auto at=allocate_heap(m,value.size()+1);if(!at)throw std::bad_alloc{};blocks.push_back(at);m.write(GuestAddress{at},std::span(reinterpret_cast<const std::uint8_t*>(value.data()),value.size()));m.store(GuestAddress{at+value.size()},8,0);return at;};
                                const auto release=[&]{for(const auto block:blocks)(void)free_heap(m,block,s);blocks.clear();};
                                const auto status=allocate_heap(m,3*size+8);if(!status)throw std::bad_alloc{};blocks.push_back(status);
                                const auto report=[&](std::uint32_t error,std::uint32_t code){m.fill(GuestAddress{status},0,3*size+8);m.store(GuestAddress{status},32,2*size+8);
                                    m.store(GuestAddress{status+size},bits,put(dll.path));m.store(GuestAddress{status+2*size},32,error);m.store(GuestAddress{status+2*size+4},32,code);};
                                if(aware){
                                    report(0,0);const auto answer=co_await service_callback::call(handler,{context,0x19,status,unregister?0U:1U});
                                    if(answer==0){release();co_return 1223U;}if(answer==2){release();continue;}
                                }
                                std::uint32_t error=0,code=0;
                                const auto module=co_await service_callback::call_value(resolve("kernel32.dll","LoadLibraryW").value,{put(dll.path)});
                                if(!module){error=static_cast<std::uint32_t>(co_await service_callback::call(resolve("kernel32.dll","GetLastError").value,{}));if(!error)error=126;code=2;}
                                else{
                                    const auto invoke=[&](std::string_view symbol,std::vector<std::uint64_t> arguments)->service_callback::Task<std::optional<std::uint32_t>>{
                                        const auto entry=co_await service_callback::call_value(resolve("kernel32.dll","GetProcAddress").value,{module,put_ansi(symbol)});
                                        if(!entry)co_return std::nullopt;co_return static_cast<std::uint32_t>(co_await service_callback::call(entry,std::move(arguments)));};
                                    if(dll.flags&1U){const auto result=co_await invoke(unregister?"DllUnregisterServer":"DllRegisterServer",{});if(!result){error=127;code=3;}else if(*result>>31){error=*result;code=4;}}
                                    if(!error && (dll.flags&2U)){const auto result=co_await invoke("DllInstall",{unregister?0U:1U,dll.argument.empty()?0:put(dll.argument)});if(!result){error=127;code=3;}else if(*result>>31){error=*result;code=5;}}
                                    (void)co_await service_callback::call(resolve("kernel32.dll","FreeLibrary").value,{module});
                                }
                                if(aware){report(error,code);const auto answer=co_await service_callback::call(handler,{context,0x20,status,unregister?0U:1U});if(answer==0){release();co_return 1223U;}}
                                release();
                            }
                            co_return 0U;
                        };
                        if(!failure)failure=co_await run_dlls(true);
                        if(!failure)failure=co_await apply_registry(s,m,plan.delreg,relative);
                        if(!failure)failure=co_await apply_registry(s,m,plan.addreg,relative);
                        if(!failure)failure=co_await apply_ini(s,m,plan.inis,relative);
                        if(!failure)failure=co_await apply_registry(s,m,plan.bitreg,relative);
                        if(!failure)failure=co_await run_dlls(false);
                        for(const auto& inf:plan.copy_infs){
                            if(failure)break;const auto bytes=utf16_bytes(inf);const auto at=allocate_heap(m,bytes.size());if(!at)throw std::bad_alloc{};m.write(GuestAddress{at},bytes);
                            if(!co_await service_callback::call(copy_inf,{at,0,0,0,0,0,0,0}))failure=last_error_?last_error_:2;(void)free_heap(m,at,s);
                        }
                        finish_with(!failure,failure);co_return std::optional<std::uint32_t>{};
                    });
            }
            if(name.starts_with("SetupInstallServicesFromInfSection")){
                // (InfHandle, SectionName, Flags)：AddService=名稱,[旗標],服務安裝區段[,事件記錄區段[,記錄類型[,事件來源]]]；DelService=名稱[,旗標[,記錄類型[,事件來源]]]。
                // 服務安裝區段：DisplayName、Description、ServiceType、StartType、ErrorControl、ServiceBinary、StartName、LoadOrderGroup、Dependencies 與 AddReg／DelReg／BitReg（HKR 為服務機碼）。
                auto& handle=find_handle(arg(0));if(!arg(1))return fail(87);const auto global=u32(2);
                struct ServiceOp {bool add{};std::u16string name,display,description,binary,group,start_name,event_key;std::vector<std::u16string> dependencies;
                    std::uint32_t flags{},type{},start{},error{};std::vector<RegOp> registry,event_registry;};
                std::vector<ServiceOp> services;
                const auto setting=[&](const std::vector<std::vector<std::u16string>>& rows,std::u16string_view key)->std::optional<std::vector<std::u16string>>{
                    for(const auto& row:rows)if(fold(row[0])==key)return std::vector<std::u16string>(row.begin()+1,row.end());return std::nullopt;};
                const auto registry_of=[&](const std::vector<std::vector<std::u16string>>& rows,std::vector<RegOp>& ops){
                    for(const auto* directive:{u"delreg",u"addreg",u"bitreg"})for(const auto& row:rows)if(fold(row[0])==directive)
                        for(std::size_t i=1;i<row.size();++i)if(!row[i].empty())parse_registry(handle,row[i],fold(row[0])==u"delreg"?1U:fold(row[0])==u"addreg"?0U:2U,ops);};
                for(const auto& row:section_rows(handle,text_arg(1))){
                    const auto key=fold(row[0]);if(key!=u"addservice" && key!=u"delservice")continue;
                    ServiceOp op;op.add=key==u"addservice";op.name=row.size()>1?row[1]:std::u16string{};if(op.name.empty())throw Failure{13};
                    if(row.size()>2 && !row[2].empty()){const auto value=number_of(row[2]);if(!value)throw Failure{13};op.flags=static_cast<std::uint32_t>(*value);}
                    op.flags|=global;
                    const std::size_t event_at=op.add?4:3;
                    const auto log_type=row.size()>event_at+1 && !row[event_at+1].empty()?row[event_at+1]:std::u16string(u"Application");
                    const auto source=row.size()>event_at+2 && !row[event_at+2].empty()?row[event_at+2]:op.name;
                    op.event_key=u"SYSTEM\\CurrentControlSet\\Services\\EventLog\\"+log_type+u"\\"+source;
                    if(op.add){
                        if(row.size()<4 || row[3].empty())throw Failure{13};const auto rows=section_rows(handle,row[3]);
                        const auto required=[&](std::u16string_view field)->std::uint32_t{const auto value=setting(rows,field);if(!value || value->empty())throw Failure{13};const auto parsed=number_of(value->front());if(!parsed)throw Failure{13};return static_cast<std::uint32_t>(*parsed);};
                        op.type=required(u"servicetype");op.start=required(u"starttype");op.error=required(u"errorcontrol");
                        const auto text_of=[&](std::u16string_view field){const auto value=setting(rows,field);return value && !value->empty()?value->front():std::u16string{};};
                        op.display=text_of(u"displayname");op.description=text_of(u"description");op.binary=text_of(u"servicebinary");op.group=text_of(u"loadordergroup");op.start_name=text_of(u"startname");
                        if(const auto value=setting(rows,u"dependencies"))for(const auto& item:*value)if(!item.empty())op.dependencies.push_back(item);
                        registry_of(rows,op.registry);
                        if(row.size()>4 && !row[4].empty())registry_of(section_rows(handle,row[4]),op.event_registry);
                    }
                    services.push_back(std::move(op));
                }
                return nested_continuation(state,memory,"SetupInstallServicesFromInfSection",
                    [this,service,services,apply_registry,hkey](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                        const WindowsCall inner(s,m,service);const unsigned size=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(size*8);
                        std::vector<std::uint64_t> blocks;
                        const auto release=[&]{for(const auto block:blocks)(void)free_heap(m,block,s);blocks.clear();};
                        const auto scratch=[&](std::size_t length)->std::uint64_t{const auto at=allocate_heap(m,std::max<std::size_t>(length,1));if(!at)throw std::bad_alloc{};blocks.push_back(at);m.fill(GuestAddress{at},0,std::max<std::size_t>(length,1));return at;};
                        const auto text=[&](const std::u16string& value)->std::uint64_t{if(value.empty())return 0;const auto bytes=utf16_bytes(value);const auto at=scratch(bytes.size());m.write(GuestAddress{at},bytes);return at;};
                        const auto api=[&](std::string_view symbol,std::vector<std::uint64_t> arguments)->service_callback::Task<std::uint64_t>{
                            co_return co_await service_callback::call_value(resolve("advapi32.dll",symbol).value,std::move(arguments));};
                        const auto last_error=[&]()->service_callback::Task<std::uint32_t>{co_return static_cast<std::uint32_t>(co_await service_callback::call(resolve("kernel32.dll","GetLastError").value,{}));};
                        const auto machine=hkey(2);std::uint32_t failure=0;
                        const auto manager=co_await api("OpenSCManagerW",{0,0,0xf003f});
                        if(!manager){failure=co_await last_error();if(!failure)failure=5;}
                        for(const auto& op:services){
                            if(failure)break;
                            const auto service_handle=co_await api("OpenServiceW",{manager,text(op.name),0xf01ff});
                            if(op.add){
                                std::u16string dependencies;for(const auto& item:op.dependencies){dependencies+=item;dependencies.push_back(0);}dependencies.push_back(0);
                                const auto dependencies_at=op.dependencies.empty()?0:[&]{auto bytes=utf16_bytes(dependencies);bytes.resize(dependencies.size()*2);const auto at=scratch(bytes.size());m.write(GuestAddress{at},bytes);return at;}();
                                std::uint64_t handle_value=service_handle;bool created=false;
                                if(service_handle){
                                    // 既有服務：NOCLOBBER 旗標保留顯示名稱（0x8）、啟動類型（0x10）、錯誤控制（0x20）、載入群組（0x40）與相依（0x80）。
                                    const std::uint64_t keep=0xffffffffU;
                                    if(!co_await api("ChangeServiceConfigW",{service_handle,op.type,(op.flags&0x10U)?keep:op.start,(op.flags&0x20U)?keep:op.error,text(op.binary),
                                        (op.flags&0x40U)?0:text(op.group),0,(op.flags&0x80U)?0:dependencies_at,text(op.start_name),0,(op.flags&0x8U)?0:text(op.display)})){failure=co_await last_error();}
                                }else{
                                    handle_value=co_await api("CreateServiceW",{manager,text(op.name),text(op.display),0xf01ff,op.type,op.start,op.error,text(op.binary),text(op.group),0,dependencies_at,text(op.start_name),0});
                                    if(!handle_value)failure=co_await last_error();else created=true;
                                }
                                if(!failure && !op.description.empty() && (created || !(op.flags&0x100U))){
                                    const auto description=scratch(size);m.store(GuestAddress{description},bits,text(op.description));
                                    if(!co_await api("ChangeServiceConfig2W",{handle_value,1,description}))failure=co_await last_error();
                                }
                                if(!failure && !op.registry.empty()){
                                    const auto key_at=scratch(size);
                                    if(const auto error=static_cast<std::uint32_t>(co_await api("RegOpenKeyExW",{machine,text(u"SYSTEM\\CurrentControlSet\\Services\\"+op.name),0,0xf003f,key_at})))failure=error;
                                    else{const auto key=m.load(GuestAddress{key_at},bits);failure=co_await apply_registry(s,m,op.registry,key);(void)co_await api("RegCloseKey",{key});}
                                }
                                if(!failure && !op.event_registry.empty()){
                                    const auto key_at=scratch(size);
                                    if(const auto error=static_cast<std::uint32_t>(co_await api("RegCreateKeyExW",{machine,text(op.event_key),0,0,0,0xf003f,0,key_at,0})))failure=error;
                                    else{const auto key=m.load(GuestAddress{key_at},bits);failure=co_await apply_registry(s,m,op.event_registry,key);(void)co_await api("RegCloseKey",{key});}
                                }
                                if(!failure && (op.flags&0x800U) && handle_value && !co_await api("StartServiceW",{handle_value,0,0})){const auto error=co_await last_error();if(error!=1056)failure=error;}
                                if(handle_value)(void)co_await api("CloseServiceHandle",{handle_value});
                            }else{
                                if(service_handle){
                                    if(op.flags&0x200U){const auto status=scratch(28);(void)co_await api("ControlService",{service_handle,1,status});}
                                    if(!co_await api("DeleteService",{service_handle})){const auto error=co_await last_error();if(error!=1072)failure=error;}
                                    (void)co_await api("CloseServiceHandle",{service_handle});
                                }
                                if(!failure && (op.flags&0x4U)){const auto key=text(op.event_key);(void)co_await api("RegDeleteTreeW",{machine,key});(void)co_await api("RegDeleteKeyW",{machine,key});}
                            }
                            release();
                        }
                        if(manager)(void)co_await api("CloseServiceHandle",{manager});
                        release();last_error_=failure;inner.return_to(s,failure?0:1);co_return std::optional<std::uint32_t>{};
                    });
            }
            // InstallHinfSection(Window, ModuleHandle, CommandLine, ShowCommand)：命令列為「區段 模式 INF 路徑」；模式含 128 時以 INF 所在目錄為來源根目錄。
            // 依平台裝飾區段後以 SPINST_ALL 與預設佇列回呼安裝，接著安裝 <區段>.Services；沙箱不延後檔案操作，因此不觸發重新開機。無字尾版本為 ANSI。
            {
                const auto command=text_arg(2);std::size_t at=0;
                const auto next_token=[&]{while(at<command.size() && command[at]==u' ')++at;const auto start=at;while(at<command.size() && command[at]!=u' ')++at;return command.substr(start,at-start);};
                const auto section_name=next_token(),mode_text=next_token();auto path=command.substr(at);
                while(!path.empty() && path.front()==u' ')path.erase(0,1);while(!path.empty() && path.back()==u' ')path.pop_back();
                if(path.size()>=2 && path.front()==u'"' && path.back()==u'"')path=path.substr(1,path.size()-2);
                if(section_name.empty() || mode_text.empty() || path.empty()){last_error_=87;return finish(0);}
                const auto mode=number_of(mode_text).value_or(0);
                State::File file;try{file=load(full_path(path),nullptr);}catch(const Failure& error){last_error_=error.code;return finish(0);}
                std::uint64_t inf=0;{const auto opened=open_handle(std::move(file));inf=opened;}
                auto& handle=find_handle(inf);
                const auto section=decorated(handle,section_name,9);const auto services_section=section+u".Services";const bool has_services=section_exists(handle,section_id(handle,services_section));
                const auto root=(mode&128U)?driver_store::parent_of(handle.files.front().path):std::u16string{};
                const auto window=arg(0);
                const auto init=resolve("setupapi.dll","SetupInitDefaultQueueCallback").value,term=resolve("setupapi.dll","SetupTermDefaultQueueCallback").value,
                    install=resolve("setupapi.dll","SetupInstallFromInfSectionW").value,services=resolve("setupapi.dll","SetupInstallServicesFromInfSectionW").value,
                    callback=resolve("setupapi.dll","SetupDefaultQueueCallbackW").value,close_inf=resolve("setupapi.dll","SetupCloseInfFile").value;
                return nested_continuation(state,memory,"InstallHinfSection",
                    [this,service,window,inf,section,services_section,has_services,root,init,term,install,services,callback,close_inf](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                        const WindowsCall inner(s,m,service);std::vector<std::uint64_t> blocks;
                        const auto text=[&](const std::u16string& value)->std::uint64_t{if(value.empty())return 0;const auto bytes=utf16_bytes(value);const auto at=allocate_heap(m,bytes.size());if(!at)throw std::bad_alloc{};blocks.push_back(at);m.write(GuestAddress{at},bytes);return at;};
                        const auto context=co_await service_callback::call_value(init,{window});
                        const auto installed=co_await service_callback::call(install,{window,inf,text(section),0x7ff,0,text(root),0x4,callback,context,0,0});
                        if(installed && has_services)(void)co_await service_callback::call(services,{inf,text(services_section),0});
                        if(context)(void)co_await service_callback::call(term,{context});
                        (void)co_await service_callback::call(close_inf,{inf});
                        for(const auto block:blocks)(void)free_heap(m,block,s);
                        inner.return_to(s,0);co_return std::optional<std::uint32_t>{};
                    });
            }
        }
