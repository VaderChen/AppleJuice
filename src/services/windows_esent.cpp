#include "windows_esent_state.hpp"
#include "x64arm/windows_paths.hpp"
#include <bit>

namespace x64arm {
std::span<const WindowsServiceDescriptor> esent_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"esent.dll","JetCreateInstanceA",2},{"esent.dll","JetCreateInstanceW",2},
        {"esent.dll","JetCreateInstance2A",4},{"esent.dll","JetCreateInstance2W",4},
        {"esent.dll","JetInit",1},{"esent.dll","JetInit2",2},{"esent.dll","JetTerm",1},{"esent.dll","JetTerm2",2},
        {"esent.dll","JetSetSystemParameterA",5},{"esent.dll","JetSetSystemParameterW",5},
        {"esent.dll","JetGetSystemParameterA",6},{"esent.dll","JetGetSystemParameterW",6},
        {"esent.dll","JetBeginSessionA",4},{"esent.dll","JetBeginSessionW",4},{"esent.dll","JetEndSession",2},{"esent.dll","JetDupSession",2},
        {"esent.dll","JetAttachDatabaseA",3},{"esent.dll","JetAttachDatabaseW",3},
        {"esent.dll","JetAttachDatabase2A",4},{"esent.dll","JetAttachDatabase2W",4},
        {"esent.dll","JetDetachDatabaseA",2},{"esent.dll","JetDetachDatabaseW",2},
        {"esent.dll","JetDetachDatabase2A",3},{"esent.dll","JetDetachDatabase2W",3},
        {"esent.dll","JetOpenDatabaseA",5},{"esent.dll","JetOpenDatabaseW",5},{"esent.dll","JetCloseDatabase",3},
        {"esent.dll","JetOpenTableA",7},{"esent.dll","JetOpenTableW",7},{"esent.dll","JetCloseTable",2},{"esent.dll","JetDupCursor",4},
        {"esent.dll","JetGetTableColumnInfoA",6},{"esent.dll","JetGetTableColumnInfoW",6},
        {"esent.dll","JetGetColumnInfoA",7},{"esent.dll","JetGetColumnInfoW",7},
        {"esent.dll","JetMove",4},{"esent.dll","JetRetrieveColumn",8},{"esent.dll","JetRetrieveColumns",4},
        {"esent.dll","JetGetBookmark",5},{"esent.dll","JetGotoBookmark",4}
    };return table;
}
EsentState& WindowsServices::esent_bridge(){if(!esent_)esent_=std::make_shared<EsentState>();else if(esent_.use_count()>1)esent_=std::make_shared<EsentState>(*esent_);return *esent_;}
bool WindowsServices::same_esent(const WindowsServices& other)const{return !esent_ || !other.esent_?esent_==other.esent_:*esent_==*other.esent_;}

std::optional<std::uint32_t> WindowsServices::invoke_esent(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    using ese_api::require;const WindowsCall args(state,memory,service);const auto name=service.symbol;const bool wide=name.ends_with('W');const auto saved=last_error_;
    const std::uint8_t bits=x86_?32:64;const auto d=[&](unsigned i){return static_cast<std::uint32_t>(args(i));};
    const auto text=[&](unsigned i){return ese_api::input(memory,args(i),wide);};
    const auto done=[&](std::int32_t result){last_error_=saved;args.return_to(state,static_cast<std::uint32_t>(result));return std::optional<std::uint32_t>{};};
    try{
        auto& bridge=esent_bridge();
        const auto path=[&](unsigned i){auto value=text(i);require(!value.empty(),-1217);return full_windows_environment_path(std::move(value),runtime_.current_directory,runtime_.environment);};
        const auto output_handle=[&](unsigned i){ese_api::writable(memory,args(i),bits/8);};
        if(name.starts_with("JetSetSystemParameter") || name.starts_with("JetGetSystemParameter")){
            const bool set=name.starts_with("JetSet");const auto id=d(2);std::uint64_t instance{};
            const auto nil=x86_?std::uint64_t{UINT32_MAX}:UINT64_MAX;
            if(args(1) && args(1)!=nil)instance=bridge.session(args(1)).instance;
            else if(set && args(0))instance=memory.load(GuestAddress{args(0)},bits);
            else if(!set)instance=args(0);
            if(instance==nil)instance=0;
            if(!instance && !bridge.multi_instance)instance=bridge.legacy_instance;
            auto* inst=instance?&bridge.instance(instance):nullptr;
            if(id==104){
                if(set){require(args(3)>=1 && args(3)<=1024);require(bridge.instances.empty() || args(3)==bridge.maximum_instances,-1030);bridge.maximum_instances=static_cast<std::uint32_t>(args(3));}
                else {ese_api::writable(memory,args(3),bits/8);memory.store(GuestAddress{args(3)},bits,bridge.maximum_instances);}return done(0);
            }
            if(id==64){
                if(set){const auto value=args(3);require(value==2048 || value==4096 || value==8192 || value==16384 || value==32768);
                    require(bridge.instances.empty() || value==bridge.page_size,-1030);bridge.page_size=static_cast<std::uint32_t>(value);}
                else {ese_api::writable(memory,args(3),bits/8);memory.store(GuestAddress{args(3)},bits,bridge.page_size);}return done(0);
            }
            if(id==70){
                require(!set && args(3));const auto error=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{args(3)},bits)));
                static const std::map<std::int32_t,std::u16string_view> names{
                    {0,u"JET_errSuccess"},{1004,u"JET_wrnColumnNull"},{1006,u"JET_wrnBufferTruncated"},{1007,u"JET_wrnDatabaseAttached"},{1813,u"JET_wrnFileOpenReadOnly"},
                    {-550,u"JET_errDatabaseDirtyShutdown"},{-900,u"JET_errInvalidGrbit"},{-1001,u"JET_errFeatureNotAvailable"},{-1002,u"JET_errInvalidName"},{-1003,u"JET_errInvalidParameter"},
                    {-1008,u"JET_errDatabaseFileReadOnly"},{-1010,u"JET_errInvalidDatabaseId"},{-1011,u"JET_errOutOfMemory"},{-1013,u"JET_errOutOfCursors"},{-1018,u"JET_errReadVerifyFailure"},
                    {-1022,u"JET_errDiskIO"},{-1028,u"JET_errInvalidDatabase"},{-1029,u"JET_errNotInitialized"},{-1030,u"JET_errAlreadyInitialized"},{-1032,u"JET_errFileAccessDenied"},
                    {-1038,u"JET_errBufferTooSmall"},{-1086,u"JET_errInstanceNameInUse"},{-1101,u"JET_errOutOfSessions"},{-1104,u"JET_errInvalidSesid"},{-1115,u"JET_errInvalidInstance"},
                    {-1202,u"JET_errDatabaseInUse"},{-1203,u"JET_errDatabaseNotFound"},{-1206,u"JET_errDatabaseCorrupted"},{-1207,u"JET_errDatabaseLocked"},{-1213,u"JET_errPageSizeMismatch"},
                    {-1217,u"JET_errDatabaseInvalidPath"},{-1302,u"JET_errTableLocked"},{-1305,u"JET_errObjectNotFound"},{-1310,u"JET_errInvalidTableId"},{-1311,u"JET_errTooManyOpenTables"},
                    {-1507,u"JET_errColumnNotFound"},{-1601,u"JET_errRecordNotFound"},{-1603,u"JET_errNoCurrentRecord"},{-1811,u"JET_errFileNotFound"},{-1910,u"JET_errSessionSharingViolation"},{-1916,u"JET_errOneDatabasePerSession"}};
                const auto found=names.find(error);require(found!=names.end());return done(ese_api::text_output(memory,found->second,wide,args(4),d(5)));
            }
            auto& params=inst?inst->parameters:bridge.defaults;const auto found=params.find(id);require(found!=params.end());auto& parameter=found->second;
            const bool string=id<=3 || id==34;
            if(set){
                require(!inst || !inst->initialized,-1030);
                if(string){auto value=text(4);require(!value.empty());
                    if(id==34){value=ese_api::folded(std::move(value));require(value==u"on" || value==u"off");}
                    else if(id==3){require(value.size()==3 && value.find_first_of(u"\\/:.*?\"")==value.npos);}
                    else {value=full_windows_environment_path(std::move(value),runtime_.current_directory,runtime_.environment);if(id!=1 && value.back()!=u'\\')value+=u'\\';}
                    parameter.text=std::move(value);
                }else {require(args(3)<=(id==102?1U:id==5?30000U:2147483647U));parameter.number=args(3);}return done(0);
            }
            if(string)return done(ese_api::text_output(memory,parameter.text,wide,args(4),d(5)));
            ese_api::writable(memory,args(3),bits/8);memory.store(GuestAddress{args(3)},bits,parameter.number);return done(0);
        }
        if(name.starts_with("JetCreateInstance")){
            output_handle(0);const bool extended=name.starts_with("JetCreateInstance2");if(extended)require(d(3)==0,-900);
            require(!bridge.legacy_instance,-1080);auto instance_name=text(1);require(instance_name.size()<=255,-1002);
            for(const auto& [id,inst]:bridge.instances){(void)id;require(ese_api::folded(inst.name)!=ese_api::folded(instance_name),-1086);}
            require(bridge.instances.size()<bridge.maximum_instances,-1214);EsentState::Instance inst;inst.name=std::move(instance_name);inst.display=extended?text(2):inst.name;inst.parameters=bridge.defaults;
            const auto id=bridge.handle();bridge.instances.emplace(id,std::move(inst));bridge.multi_instance=true;memory.store(GuestAddress{args(0)},bits,id);return done(0);
        }
        if(name=="JetInit" || name=="JetInit2"){
            if(name=="JetInit2")require(d(1)==0,-900);if(args(0))output_handle(0);
            auto id=args(0)?memory.load(GuestAddress{args(0)},bits):0;if(id==(x86_?std::uint64_t{UINT32_MAX}:UINT64_MAX))id=0;
            if(!id){require(!bridge.multi_instance,-1081);id=bridge.legacy_instance;
                if(!id){EsentState::Instance inst;inst.parameters=bridge.defaults;id=bridge.handle();bridge.instances.emplace(id,std::move(inst));bridge.legacy_instance=id;}}
            auto& inst=bridge.instance(id);require(!inst.initialized,-1030);inst.initialized=true;if(args(0))memory.store(GuestAddress{args(0)},bits,id);return done(0);
        }
        if(name=="JetTerm" || name=="JetTerm2"){
            const auto flags=name=="JetTerm2"?d(1):0U;require(!(flags&~7U) && (flags&3U)!=3U,-900);
            auto id=args(0);if(!id && !bridge.multi_instance)id=bridge.legacy_instance;(void)bridge.instance(id);
            for(const auto& [sid,s]:bridge.sessions){(void)sid;require(s.instance!=id || !s.busy,-1914);}
            std::erase_if(bridge.cursors,[&](const auto& entry){return entry.second.instance==id;});std::erase_if(bridge.sessions,[&](const auto& entry){return entry.second.instance==id;});
            for(auto it=bridge.documents.begin();it!=bridge.documents.end();){if(it->second.instance!=id){++it;continue;}if(it->second.file)close_transfer_handle(memory,it->second.file);it=bridge.documents.erase(it);}
            bridge.instances.erase(id);if(bridge.legacy_instance==id)bridge.legacy_instance=0;return done(0);
        }
        if(name.starts_with("JetBeginSession") || name=="JetDupSession"){
            output_handle(1);auto instance=args(0);if(name=="JetDupSession")instance=bridge.session(instance).instance;
            else {if(!instance && !bridge.multi_instance)instance=bridge.legacy_instance;}
            const auto id=bridge.add_session(instance);memory.store(GuestAddress{args(1)},bits,id);return done(0);
        }
        if(name=="JetEndSession"){require(!(d(1)&~1U),-900);(void)bridge.session(args(0));bridge.end_session(args(0));return done(0);}
        if(name.starts_with("JetAttachDatabase")){
            auto& session=bridge.session(args(0));const bool extended=name.starts_with("JetAttachDatabase2");const auto flags=d(extended?3U:2U);require(!(flags&~1U),-900);
            auto value=path(1);const auto key=ese_api::folded(value);
            for(const auto& [id,doc]:bridge.documents){(void)id;if(doc.instance==session.instance && doc.key==key)return done(1007);}
            for(const auto& [id,op]:bridge.operations){(void)id;require(op.instance!=session.instance || op.key!=key,-1202);}
            EsentState::Operation op;op.instance=session.instance;op.session=args(0);op.path=std::move(value);op.key=key;op.flags=flags;op.maximum_pages=extended?d(2):0;op.saved_error=saved;
            const auto token=bridge.next_operation++;bridge.operations.emplace(token,std::move(op));session.busy=token;return resume_esent(state,memory,token,0);
        }
        if(name.starts_with("JetDetachDatabase")){
            const auto instance=bridge.session(args(0)).instance;if(name.starts_with("JetDetachDatabase2"))require(d(2)==0,-900);
            const auto key=args(1)?ese_api::folded(path(1)):std::u16string{};std::vector<std::uint32_t> selected;
            for(const auto& [id,doc]:bridge.documents)if(doc.instance==instance && (key.empty() || doc.key==key))selected.push_back(id);
            require(key.empty() || !selected.empty(),-1203);
            for(const auto id:selected)for(const auto& [sid,s]:bridge.sessions){(void)sid;require(!s.databases.contains(id),-1202);}
            for(const auto id:selected){auto& doc=bridge.documents.at(id);if(doc.file)close_transfer_handle(memory,doc.file);bridge.documents.erase(id);}return done(0);
        }
        if(name.starts_with("JetOpenDatabase")){
            auto& session=bridge.session(args(0));ese_api::writable(memory,args(3),4);require(!args(2));const auto flags=d(4);require(!(flags&~3U),-900);const auto key=ese_api::folded(path(1));
            auto found=std::find_if(bridge.documents.begin(),bridge.documents.end(),[&](const auto& entry){return entry.second.instance==session.instance && entry.second.key==key;});require(found!=bridge.documents.end(),-1203);
            const auto id=found->first;const auto& doc=found->second;
            require(!bridge.instance(session.instance).parameters.at(102).number || session.databases.empty() || session.databases.contains(id),-1916);
            for(const auto& [sid,s]:bridge.sessions){if(sid==args(0))continue;const auto other=s.databases.find(id);if(other!=s.databases.end()){require(!(other->second.flags&2),-1207);require(!(flags&2),-1202);}}
            auto& open=session.databases[id];require(open.references<UINT32_MAX,-1011);++open.references;open.flags|=flags|(doc.flags&1);memory.store(GuestAddress{args(3)},32,id);return done((doc.flags&1) && !(flags&1)?1813:0);
        }
        if(name=="JetCloseDatabase"){
            require(d(2)==0,-900);(void)bridge.document(args(0),d(1));auto& session=bridge.session(args(0));auto& open=session.databases.at(d(1));
            open.flags&=~2U;
            if(--open.references==0){session.databases.erase(d(1));std::erase_if(bridge.cursors,[&](const auto& entry){return entry.second.session==args(0) && entry.second.database==d(1);});}return done(0);
        }
        return invoke_esent_cursor(state,memory,service);
    }catch(const ese_api::Error& error){return done(error.code);}catch(const esent::Error& error){return done(ese_api::format_error(error));}
    catch(const MemoryFault&){return done(-1003);}catch(const std::bad_alloc&){return done(-1011);}catch(const std::invalid_argument&){return done(-1003);}
}
}
