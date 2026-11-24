// DbgHelp 行號與原始檔（由 windows_dbghelp_symbols.cpp 引入，位於 x64arm 命名空間）：資料來自與映像 GUID／Age 相符的 PDB（pdb_reader.hpp）。
// - SymGetLineFromAddr(64／W64)、SymGetLineFromInlineContext(W)：位址落在某一行的程式碼範圍內時回傳該行與位移，否則 ERROR_INVALID_ADDRESS。
//   沒有行內框架資訊，行內內容與一般位址相同。IMAGEHLP_LINE 的 Key 為行號表索引＋1，FileName 指向模組持有的字串（卸載時釋放）。
// - SymGetLineFromName(64／W64)：在符合的原始檔中找指定行（沒有時取之後最近的一行），plDisplacement 為行數差。
// - SymGetLineNext／Prev(64／W64)：同一原始檔中依位址的下一／上一行；沒有時 ERROR_NO_MORE_ITEMS。
// - SymEnumLines(W)、SymEnumSourceLines(W)（ESLFLAG_FULLPATH／NEAREST／PREV／NEXT）、SymEnumSourceFiles(W)：依行號表順序回呼，回呼回 FALSE 即停止。
// - SymQueryInlineTrace：沒有行內框架，目前內容為 0、框架索引 0。SymGetSourceFileToken(W)：PDB 的 srcsrv 串流中列出的原始檔回傳該列為 token。
// - ExtensionApiVersion／WinDbgExtensionDllInit：DbgHelp 作為偵錯器延伸模組時的版本與 WINDBG_EXTENSION_APIS 登記。
namespace {
bool dbghelp_line_service(std::string_view name){
    for(const auto prefix:{"SymGetLineFrom","SymGetLineNext","SymGetLinePrev","SymEnumLines","SymEnumSourceLines","SymEnumSourceFiles","SymQueryInlineTrace","SymGetSourceFileToken"})
        if(name.starts_with(prefix))return true;
    return name=="ExtensionApiVersion" || name=="WinDbgExtensionDllInit";
}
bool line_file_match(std::string_view path,std::u16string_view wanted,bool full){
    const auto text=utf8_to_utf16(std::string(path));
    const auto equal=[](std::u16string_view a,std::u16string_view b){return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](char16_t x,char16_t y){
        const auto fold=[](char16_t c){return c==u'/'?u'\\':unicode_case_unit(c,true);};return fold(x)==fold(y);});};
    if(equal(text,wanted))return true;if(full)return false;
    if(wanted.find_first_of(u"*?[")!=wanted.npos)return symbol_match(text,wanted,false) || symbol_match(leaf(text),wanted,false);
    if(text.size()>wanted.size() && equal(std::u16string_view(text).substr(text.size()-wanted.size()),wanted)){const auto c=text[text.size()-wanted.size()-1];return c==u'\\' || c==u'/';}
    return false;
}
}
std::optional<std::uint32_t> WindowsServices::invoke_dbghelp_lines(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const bool wide=name.ends_with('W') || name.ends_with("W64");
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto fail=[&](std::uint32_t error){last_error_=error;return finish(0);};
    const auto text=[&](std::uint64_t pointer){return wide?read_utf16(memory,GuestAddress{pointer},32768):ansi_to_utf16(read_ansi(memory,pointer,32768));};
    if(name=="ExtensionApiVersion"){
        // EXT_API_VERSION {MajorVersion, MinorVersion, Revision（EXT_API_VERSION_NUMBER64）, Reserved}，DbgHelp 持有的靜態區塊。
        if(!dbghelp_extension_version_){const auto block=allocate_heap(memory,8);if(!block)return finish(0);
            memory.store(GuestAddress{block},16,10);memory.store(GuestAddress{block+2},16,0);memory.store(GuestAddress{block+4},16,6);memory.store(GuestAddress{block+6},16,0);dbghelp_extension_version_=block;}
        return finish(dbghelp_extension_version_);
    }
    if(name=="WinDbgExtensionDllInit"){
        // (PWINDBG_EXTENSION_APIS, MajorVersion, MinorVersion)：保存偵錯器提供的回呼表（第一個 ULONG 為 nSize）與版本。
        dbghelp_extension_apis_.clear();
        if(arg(0)){const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)},32));if(size && size<=4096){dbghelp_extension_apis_.resize(size);memory.read(GuestAddress{arg(0)},dbghelp_extension_apis_);}}
        dbghelp_extension_debugger_={static_cast<std::uint32_t>(arg(1))&0xffffU,static_cast<std::uint32_t>(arg(2))&0xffffU};
        return finish(0);
    }
    const auto found=runtime_.symbol_sessions.find(arg(0));if(found==runtime_.symbol_sessions.end())return fail(6);
    auto& session=found->second;
    try{
        // IMAGEHLP_LINE（32 位元位址）、IMAGEHLP_LINE64／LINEW64：SizeOfStruct、Key、LineNumber、FileName、Address。
        const bool legacy=name=="SymGetLineFromAddr" || name=="SymGetLineFromName" || name=="SymGetLineNext" || name=="SymGetLinePrev";
        const unsigned line_size=x86_?(legacy?20U:24U):40U;
        const auto file_text=[&](SymbolModule& module,std::uint32_t index)->std::uint64_t{
            auto& cache=wide?module.line_text_wide:module.line_text;if(const auto hit=cache.find(index);hit!=cache.end())return hit->second;
            const auto& path=module.pdb->files.at(index);std::vector<std::uint8_t> bytes;
            if(wide)bytes=utf16_bytes(utf8_to_utf16(path));else{bytes.assign(path.begin(),path.end());bytes.push_back(0);}
            const auto block=allocate_heap(memory,bytes.size());if(!block)throw SymbolFailure{8};memory.write(GuestAddress{block},bytes);cache.emplace(index,block);return block;};
        const auto check_line=[&](std::uint64_t pointer){if(!pointer)throw SymbolFailure{87};if(memory.load(GuestAddress{pointer},32)!=line_size)throw SymbolFailure{87};memory.check(GuestAddress{pointer},line_size,Access::Write);};
        const auto write_line=[&](std::uint64_t pointer,SymbolModule& module,std::size_t index){
            const auto& entry=module.pdb->lines.at(index);
            memory.store(GuestAddress{pointer+unit},bits,index+1);memory.store(GuestAddress{pointer+unit*2},32,entry.line);
            memory.store(GuestAddress{pointer+unit*3},bits,file_text(module,entry.file));
            const auto address=module.image.base+entry.rva;
            if(legacy)memory.store(GuestAddress{pointer+unit*4},32,address);else memory.store(GuestAddress{pointer+(x86_?16U:32U)},64,address);};
        const auto line_at=[&](SymbolModule& module,std::uint64_t address)->std::optional<std::size_t>{
            if(!module.pdb || address<module.image.base)return std::nullopt;const auto rva=address-module.image.base;const auto& lines=module.pdb->lines;
            auto it=std::upper_bound(lines.begin(),lines.end(),rva,[](std::uint64_t value,const pdb::Line& line){return value<line.rva;});
            while(it!=lines.begin()){--it;if(rva<it->end)return static_cast<std::size_t>(it-lines.begin());if(it->rva<rva && it->end<=rva)break;}
            return std::nullopt;};
        if(name.starts_with("SymGetLineFromAddr") || name.starts_with("SymGetLineFromInlineContext")){
            // SymGetLineFromAddr(64)(hProcess, Address, pdwDisplacement, Line)；InlineContext 版 (hProcess, Address, InlineContext, ModuleBase, pdwDisplacement, Line)。
            const bool inline_context=name.starts_with("SymGetLineFromInlineContext");
            const auto address=legacy?(arg(1)&UINT32_MAX):arg(1);const auto displacement=arg(inline_context?4:2),line=arg(inline_context?5:3);
            check_line(line);if(!displacement)throw SymbolFailure{87};
            auto* module=containing(session,address);if(!module || (inline_context && arg(3) && arg(3)!=module->image.base))throw SymbolFailure{487};materialize(*module);
            const auto index=line_at(*module,address);if(!index)throw SymbolFailure{487};
            write_line(line,*module,*index);memory.store(GuestAddress{displacement},32,address-(module->image.base+module->pdb->lines[*index].rva));return finish(1);
        }
        if(name.starts_with("SymGetLineFromName")){
            // (hProcess, ModuleName, FileName, dwLineNumber, plDisplacement, Line)
            const auto module_name=arg(1)?text(arg(1)):std::u16string{};if(!arg(2))throw SymbolFailure{87};const auto file=text(arg(2));const auto wanted=static_cast<std::uint32_t>(arg(3));
            check_line(arg(5));if(!arg(4))throw SymbolFailure{87};
            bool file_seen=false;
            for(auto& [base,module]:session.images){(void)base;
                if(!module_name.empty() && !symbol_match(module.name,module_name,false))continue;materialize(module);if(!module.pdb)continue;
                std::optional<std::size_t> best;
                for(std::size_t i=0;i<module.pdb->lines.size();++i){const auto& entry=module.pdb->lines[i];
                    if(!line_file_match(module.pdb->files[entry.file],file,false))continue;file_seen=true;if(entry.line<wanted)continue;
                    if(!best || entry.line<module.pdb->lines[*best].line)best=i;}
                if(best){write_line(arg(5),module,*best);memory.store(GuestAddress{arg(4)},32,module.pdb->lines[*best].line-wanted);return finish(1);}
            }
            throw SymbolFailure{file_seen?1168U:2U};
        }
        if(name.starts_with("SymGetLineNext") || name.starts_with("SymGetLinePrev")){
            const bool next=name.starts_with("SymGetLineNext");check_line(arg(1));
            const auto address=legacy?memory.load(GuestAddress{arg(1)+unit*4},32):memory.load(GuestAddress{arg(1)+(x86_?16U:32U)},64);
            auto* module=containing(session,address);if(!module)throw SymbolFailure{487};materialize(*module);if(!module->pdb)throw SymbolFailure{487};
            const auto& lines=module->pdb->lines;std::optional<std::size_t> current;
            const auto key=memory.load(GuestAddress{arg(1)+unit},bits);
            if(key && key<=lines.size() && module->image.base+lines[key-1].rva==address)current=static_cast<std::size_t>(key-1);else current=line_at(*module,address);
            if(!current)throw SymbolFailure{487};const auto file=lines[*current].file;
            if(next){for(auto i=*current+1;i<lines.size();++i)if(lines[i].file==file){write_line(arg(1),*module,i);return finish(1);}}
            else for(auto i=*current;i-->0;)if(lines[i].file==file){write_line(arg(1),*module,i);return finish(1);}
            throw SymbolFailure{259};
        }
        if(name=="SymQueryInlineTrace"){
            // (hProcess, StartAddress, StartContext, StartRetAddress, CurAddress, CurContext*, CurFrameIndex*)
            if(!arg(5) || !arg(6))throw SymbolFailure{87};if(!containing(session,arg(1)))throw SymbolFailure{487};
            memory.store(GuestAddress{arg(5)},32,0);memory.store(GuestAddress{arg(6)},32,0);return finish(1);
        }
        if(name.starts_with("SymGetSourceFileToken")){
            // (hProcess, Base, FileSpec, Token*, Size*)：srcsrv 的「SRCSRV: source files」區段中第一欄與 FileSpec 相同的列。
            if(!arg(2) || !arg(3) || !arg(4))throw SymbolFailure{87};
            auto* module=session.images.contains(arg(1))?&session.images.at(arg(1)):nullptr;if(!module)throw SymbolFailure{126};materialize(*module);
            if(!module->pdb || module->pdb->source_server.empty())throw SymbolFailure{1168};
            const std::string_view data(reinterpret_cast<const char*>(module->pdb->source_server.data()),module->pdb->source_server.size());
            const auto wanted=text(arg(2));bool files=false;
            for(std::size_t at=0;at<data.size();){
                auto end=data.find('\n',at);if(end==data.npos)end=data.size();auto row=data.substr(at,end-at);if(!row.empty() && row.back()=='\r')row.remove_suffix(1);at=end+1;
                if(row.starts_with("SRCSRV: source files")){files=true;continue;}if(row.starts_with("SRCSRV:")){if(files)break;continue;}
                if(!files)continue;const auto star=row.find('*');
                if(line_file_match(row.substr(0,star),wanted,true)){
                    std::vector<std::uint8_t> token(row.begin(),row.end());token.push_back(0);const auto block=allocate_heap(memory,token.size());if(!block)throw SymbolFailure{8};
                    memory.write(GuestAddress{block},token);memory.store(GuestAddress{arg(3)},bits,block);memory.store(GuestAddress{arg(4)},32,token.size());return finish(1);
                }
            }
            throw SymbolFailure{1168};
        }
        // 列舉：先實體化模組（可能送出延遲載入事件），回呼在協程中依序執行。
        const bool files_only=name.starts_with("SymEnumSourceFiles"),source_lines=name.starts_with("SymEnumSourceLines");
        std::vector<SymbolModule*> modules;
        if(arg(1)){auto* module=session.images.contains(arg(1))?&session.images.at(arg(1)):nullptr;if(!module)throw SymbolFailure{126};modules.push_back(module);}
        else for(auto& [base,module]:session.images){(void)base;modules.push_back(&module);}
        for(auto* module:modules)materialize(*module);
        const auto callback=arg(files_only?3:source_lines?6:4),context=arg(files_only?4:source_lines?7:5);if(!callback)throw SymbolFailure{87};
        const auto object=files_only?std::u16string{}:(arg(2)?text(arg(2)):std::u16string{});
        const auto file_filter=files_only?(arg(2)?text(arg(2)):std::u16string{}):(arg(3)?text(arg(3)):std::u16string{});
        const auto wanted_line=source_lines?static_cast<std::uint32_t>(arg(4)):0U,flags=source_lines?static_cast<std::uint32_t>(arg(5)):0U;
        if(flags&~0x1fU)throw SymbolFailure{87};
        struct Item {SymbolModule* module;std::size_t index;};std::vector<Item> items;std::vector<std::pair<SymbolModule*,std::uint32_t>> sources;
        for(auto* module:modules){
            if(!module->pdb)continue;const auto& db=*module->pdb;
            if(files_only){for(std::uint32_t i=0;i<db.files.size();++i)if(file_filter.empty() || file_filter==u"*" || line_file_match(db.files[i],file_filter,false))sources.emplace_back(module,i);continue;}
            std::vector<std::size_t> matched;
            for(std::size_t i=0;i<db.lines.size();++i){const auto& entry=db.lines[i];
                if(!object.empty() && !line_file_match(db.objects.at(entry.module),object,false))continue;
                if(!file_filter.empty() && !line_file_match(db.files[entry.file],file_filter,(flags&1U)!=0))continue;matched.push_back(i);}
            if(source_lines && wanted_line){
                std::vector<std::size_t> exact;for(const auto i:matched)if(db.lines[i].line==wanted_line)exact.push_back(i);
                if(exact.empty() && (flags&0xeU)){
                    std::optional<std::uint32_t> pick;
                    for(const auto i:matched){const auto value=db.lines[i].line;
                        if(((flags&2U) || (flags&8U)) && value>wanted_line && (!pick || value<*pick))pick=value;
                        if((flags&4U) && !(flags&0xaU) && value<wanted_line && (!pick || value>*pick))pick=value;}
                    if(pick)for(const auto i:matched)if(db.lines[i].line==*pick)exact.push_back(i);
                }
                matched=std::move(exact);
            }
            for(const auto i:matched)items.push_back({module,i});
        }
        // SRCCODEINFO(W)：SizeOfStruct、Key、ModBase、Obj[MAX_PATH+1]、FileName[MAX_PATH+1]、LineNumber、Address。
        const unsigned chars=wide?2U:1U,obj_at=x86_?16U:24U,file_at=obj_at+261*chars,line_at_offset=(file_at+261*chars+3)&~3U,address_at=(line_at_offset+4+7)&~7U,code_size=(address_at+8+7)&~7U;
        struct Record {std::vector<std::uint8_t> bytes;std::uint64_t key{};};std::vector<Record> records;std::vector<std::vector<std::uint8_t>> names;
        const auto fixed=[&](std::vector<std::uint8_t>& bytes,unsigned at,const std::string& value){
            if(wide){const auto utf16=utf8_to_utf16(value);for(std::size_t i=0;i<std::min<std::size_t>(utf16.size(),260);++i){bytes[at+i*2]=static_cast<std::uint8_t>(utf16[i]);bytes[at+i*2+1]=static_cast<std::uint8_t>(utf16[i]>>8);}}
            else std::copy_n(value.begin(),std::min<std::size_t>(value.size(),260),bytes.begin()+at);};
        if(files_only)for(const auto& [module,index]:sources){
            const auto& path=module->pdb->files[index];std::vector<std::uint8_t> value;if(wide)value=utf16_bytes(utf8_to_utf16(path));else{value.assign(path.begin(),path.end());value.push_back(0);}
            names.push_back(std::move(value));std::vector<std::uint8_t> record(16);for(unsigned b=0;b<8;++b)record[b]=static_cast<std::uint8_t>(module->image.base>>(8*b));records.push_back({std::move(record),0});
        }else for(const auto& item:items){
            const auto& entry=item.module->pdb->lines[item.index];std::vector<std::uint8_t> bytes(code_size);
            const auto put=[&](unsigned at,std::uint64_t value,unsigned size){for(unsigned b=0;b<size;++b)bytes[at+b]=static_cast<std::uint8_t>(value>>(8*b));};
            put(0,code_size,4);put(unit,item.index+1,unit);put(x86_?8U:16U,item.module->image.base,8);fixed(bytes,obj_at,item.module->pdb->objects.at(entry.module));
            fixed(bytes,file_at,item.module->pdb->files[entry.file]);put(line_at_offset,entry.line,4);put(address_at,item.module->image.base+entry.rva,8);records.push_back({std::move(bytes),0});
        }
        if(records.empty())return finish(1);
        return nested_continuation(state,memory,"DbgHelp 行號列舉",[this,service,records=std::move(records),names=std::move(names),callback,context,files_only,unit](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);
            const auto block=allocate_heap(m,4096);if(!block){last_error_=8;call.return_to(s,0);co_return std::nullopt;}
            struct Release {WindowsServices& self;GuestMemory& m;GuestState& s;std::uint64_t block;~Release(){(void)self.free_heap(m,block,s);}} release{*this,m,s,block};
            for(std::size_t i=0;i<records.size();++i){
                if(files_only){
                    const auto& value=names[i];const auto text=block+16;if(value.size()>4096-16)continue;
                    m.write(GuestAddress{block},records[i].bytes);m.write(GuestAddress{text},value);m.store(GuestAddress{block+8},static_cast<std::uint8_t>(unit*8),text);
                }else m.write(GuestAddress{block},records[i].bytes);
                if(!(co_await service_callback::call_value(callback,{block,context})&UINT32_MAX))break;
            }
            call.return_to(s,1);co_return std::nullopt;
        });
    }catch(const SymbolFailure& error){return fail(error.error);}catch(const std::bad_alloc&){return fail(8);}catch(const std::out_of_range&){return fail(13);}
}
