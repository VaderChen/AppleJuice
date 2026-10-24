#include "windows_msi_state.hpp"
#include <algorithm>
#include <bit>

namespace x64arm {
std::span<const WindowsServiceDescriptor> msi_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"msi.dll","MsiCloseHandle",1},{"msi.dll","MsiCloseAllHandles",0},{"msi.dll","MsiCreateRecord",1},
        {"msi.dll","MsiRecordClearData",1},{"msi.dll","MsiRecordGetFieldCount",1},{"msi.dll","MsiRecordDataSize",2},{"msi.dll","MsiRecordIsNull",2},
        {"msi.dll","MsiRecordGetInteger",2},{"msi.dll","MsiRecordSetInteger",3},{"msi.dll","MsiRecordGetStringA",4},{"msi.dll","MsiRecordGetStringW",4},
        {"msi.dll","MsiRecordSetStringA",3},{"msi.dll","MsiRecordSetStringW",3},{"msi.dll","MsiRecordReadStream",4},
        {"msi.dll","MsiRecordSetStreamA",3},{"msi.dll","MsiRecordSetStreamW",3},
        {"msi.dll","MsiOpenDatabaseA",3},{"msi.dll","MsiOpenDatabaseW",3},{"msi.dll","MsiDatabaseCommit",1},{"msi.dll","MsiGetDatabaseState",1},
        {"msi.dll","MsiDatabaseOpenViewA",3},{"msi.dll","MsiDatabaseOpenViewW",3},{"msi.dll","MsiDatabaseGetPrimaryKeysA",3},{"msi.dll","MsiDatabaseGetPrimaryKeysW",3},
        {"msi.dll","MsiDatabaseIsTablePersistentA",2},{"msi.dll","MsiDatabaseIsTablePersistentW",2},
        {"msi.dll","MsiViewExecute",2},{"msi.dll","MsiViewFetch",2},{"msi.dll","MsiViewClose",1},{"msi.dll","MsiViewGetColumnInfo",3},
        {"msi.dll","MsiGetProductInfoA",4},{"msi.dll","MsiGetProductInfoW",4},{"msi.dll","MsiGetComponentPathA",4},{"msi.dll","MsiGetComponentPathW",4}
    };return table;
}
MsiState& WindowsServices::msi_bridge(){if(!msi_)msi_=std::make_shared<MsiState>();else if(msi_.use_count()>1)msi_=std::make_shared<MsiState>(*msi_);return *msi_;}
bool WindowsServices::same_msi(const WindowsServices& other)const{return !msi_ || !other.msi_?msi_==other.msi_:*msi_==*other.msi_;}
void WindowsServices::collect_msi(GuestMemory& memory){
    auto& bridge=msi_bridge();for(auto it=bridge.documents.begin();it!=bridge.documents.end();){const auto id=it->first;
        if(it->second.busy || std::any_of(bridge.objects.begin(),bridge.objects.end(),[&](const auto& object){return object.second.document==id;})){++it;continue;}
        if(it->second.file)close_transfer_handle(memory,it->second.file);it=bridge.documents.erase(it);
    }
}
std::optional<std::uint32_t> WindowsServices::invoke_msi(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall args(state,memory,service);const auto name=service.symbol;const bool wide=name.ends_with('W');const auto saved_error=last_error_;
    const auto d=[&](unsigned i){return static_cast<std::uint32_t>(args(i));};
    const auto done=[&](std::uint64_t result){last_error_=saved_error;args.return_to(state,result);return std::optional<std::uint32_t>{};};
    using Type=MsiState::Type;using Kind=MsiState::Kind;
    try{
        auto& bridge=msi_bridge();
        const auto record=[&](std::vector<msi::Cell> fields,std::uint64_t output){
            if(!output)throw msi::Error{87,"MSI 輸出指標無效"};memory.check(GuestAddress{output},4,Access::Write);
            MsiState::Object object;object.owner=threads_.current;object.fields=std::move(fields);const auto id=bridge.add(std::move(object));memory.store(GuestAddress{output},32,id);return id;
        };
        const auto start=[&](MsiState::Operation op){op.saved_error=saved_error;op.owner=threads_.current;const auto id=bridge.next_operation++;bridge.operations.emplace(id,std::move(op));return resume_msi(state,memory,id,0);};
        if(name.starts_with("MsiGetProductInfo") || name.starts_with("MsiGetComponentPath"))return begin_msi_registry(state,memory,service);
        if(name=="MsiCloseHandle"){
            if(!bridge.objects.erase(d(0)))return done(6);collect_msi(memory);return done(0);
        }
        if(name=="MsiCloseAllHandles"){
            const auto count=std::erase_if(bridge.objects,[&](const auto& entry){return entry.second.owner==threads_.current;});collect_msi(memory);return done(count);
        }
        if(name=="MsiCreateRecord"){
            if(d(0)>65535)return done(0);MsiState::Object object;object.owner=threads_.current;object.fields.resize(static_cast<std::size_t>(d(0))+1);return done(bridge.add(std::move(object)));
        }
        if(name.starts_with("MsiRecord")){
            const auto found=bridge.objects.find(d(0));const bool valid=found!=bridge.objects.end() && found->second.type==Type::Record;
            if(!valid){if(name=="MsiRecordGetInteger")return done(0x80000000U);if(name=="MsiRecordIsNull")return done(1);if(name=="MsiRecordDataSize" || name=="MsiRecordGetFieldCount")return done(0);return done(6);}
            auto& object=found->second;
            if(name=="MsiRecordGetFieldCount")return done(object.fields.size()-1);
            if(name=="MsiRecordClearData"){for(auto& cell:object.fields)cell={};object.positions.clear();return done(0);}
            const auto field=d(1);const msi::Cell null;const auto& cell=field<object.fields.size()?object.fields[field]:null;
            if(name=="MsiRecordIsNull")return done(std::holds_alternative<std::monostate>(cell));
            if(name=="MsiRecordGetInteger"){const auto value=msi::cell_integer(cell);return done(value?static_cast<std::uint32_t>(*value):0x80000000U);}
            if(name=="MsiRecordDataSize"){
                if(std::holds_alternative<std::int32_t>(cell))return done(4);if(const auto* text=std::get_if<std::u16string>(&cell))return done(text->size());if(const auto* bytes=std::get_if<msi::Binary>(&cell))return done(bytes->size());return done(0);
            }
            if(name.starts_with("MsiRecordGetString")){if(!args(3))return done(87);return done(msi_api::output(memory,msi::cell_text(cell),wide,args(2),args(3)));}
            if(name=="MsiRecordReadStream"){
                if(!args(3))return done(87);const auto* bytes=std::get_if<msi::Binary>(&cell);if(!bytes)return done(1627);
                memory.check(GuestAddress{args(3)},4,Access::Write);const auto count=memory.load(GuestAddress{args(3)},32);auto& position=object.positions[field];const auto remaining=bytes->size()-std::min(position,bytes->size());
                const auto copied=args(2)?std::min<std::size_t>(static_cast<std::size_t>(count),remaining):remaining;
                if(args(2)){memory.write(GuestAddress{args(2)},std::span(*bytes).subspan(position,copied));position+=copied;}memory.store(GuestAddress{args(3)},32,copied);return done(0);
            }
            if(field>=object.fields.size())return done(87);
            if(name=="MsiRecordSetInteger"){object.fields[field]=d(2)==0x80000000U?msi::Cell{}:msi::Cell{std::bit_cast<std::int32_t>(d(2))};object.positions.erase(field);return done(0);}
            if(name.starts_with("MsiRecordSetString")){const auto text=msi_api::input(memory,args(2),wide);object.fields[field]=text.empty()?msi::Cell{}:msi::Cell{text};object.positions.erase(field);return done(0);}
            if(name.starts_with("MsiRecordSetStream")){
                if(!args(2)){if(!std::holds_alternative<msi::Binary>(cell))return done(1627);object.positions[field]=0;return done(0);}
                MsiState::Operation op;op.kind=Kind::Stream;op.path=msi_api::input(memory,args(2),wide);op.record=d(0);op.field=field;if(op.path.empty())return done(110);return start(std::move(op));
            }
        }
        if(name.starts_with("MsiOpenDatabase")){
            if(!args(0) || !args(2))return done(87);memory.store(GuestAddress{args(2)},32,0);
            MsiState::Operation op;op.kind=Kind::Open;op.path=msi_api::input(memory,args(0),wide);op.output=args(2);
            if(args(1)<65536){op.patch=(d(1)&0x20)!=0;op.mode=d(1)&~0x20U;if(op.mode>4)return done(87);}else {op.destination=msi_api::input(memory,args(1),wide);op.mode=1;if(op.destination.empty())return done(87);}
            if(op.path.empty())return done(110);return start(std::move(op));
        }
        if(name.starts_with("MsiView")){
            auto& view=bridge.get(d(0),Type::View);auto& document=bridge.documents.at(view.document);
            if(name=="MsiViewClose"){view.result={};view.cursor=0;view.executed=false;return done(0);}
            if(name=="MsiViewGetColumnInfo"){
                if(d(1)>1)return done(87);std::vector<msi::Cell> fields(1);for(const auto& column:view.query.columns)fields.emplace_back(d(1)?msi::column_definition(column):column.name);
                record(std::move(fields),args(2));return done(0);
            }
            if(name=="MsiViewFetch"){
                if(!args(1))return done(87);memory.store(GuestAddress{args(1)},32,0);if(!view.executed)return done(1627);if(view.cursor>=view.result.rows.size())return done(259);
                std::vector<msi::Cell> fields(1);const auto& row=view.result.rows[view.cursor].fields;fields.insert(fields.end(),row.begin(),row.end());record(std::move(fields),args(1));++view.cursor;return done(0);
            }
            if(name=="MsiViewExecute"){
                if(document.busy || document.failed || (view.query.writes && !document.mode))return done(1627);std::vector<msi::Cell> parameters;
                if(d(1)){const auto& values=bridge.get(d(1),Type::Record).fields;parameters.assign(values.begin()+1,values.end());}
                view.result={};view.cursor=0;view.executed=false;view.result=msi::execute(document.data,view.query,parameters);view.executed=true;return done(0);
            }
        }
        auto& object=bridge.get(d(0),Type::Database);auto& document=bridge.documents.at(object.document);
        if(name=="MsiGetDatabaseState")return done(document.mode?1U:0U);
        if(name=="MsiDatabaseCommit"){
            if(!document.mode)return done(0);if(document.busy || document.failed)return done(1627);MsiState::Operation op;op.kind=Kind::Commit;op.document=object.document;op.file=document.file;op.previous=document.committed;
            const auto packed=msi::serialize(document.data);auto tree=compound::parse(packed);tree.transaction=document.data.storage.transaction+1;op.bytes=compound::serialize_append(tree,op.previous);return start(std::move(op));
        }
        if(name.starts_with("MsiDatabaseIsTablePersistent")){
            if(!args(1))return done(3);const auto text=msi_api::input(memory,args(1),wide);const auto it=document.data.tables.find(text);if(it==document.data.tables.end())return done(2);return done(it->second.persistent?1U:0U);
        }
        if(name.starts_with("MsiDatabaseGetPrimaryKeys")){
            if(!args(1) || !args(2))return done(87);memory.store(GuestAddress{args(2)},32,0);const auto text=msi_api::input(memory,args(1),wide);const auto table=document.data.tables.find(text);if(table==document.data.tables.end())return done(1627);
            std::vector<msi::Cell> fields{text};for(const auto& column:table->second.columns)if(column.type&msi::key)fields.emplace_back(column.name);record(std::move(fields),args(2));return done(0);
        }
        if(name.starts_with("MsiDatabaseOpenView")){
            if(!args(1) || !args(2))return done(87);memory.store(GuestAddress{args(2)},32,0);MsiState::Object view;view.type=Type::View;view.owner=threads_.current;view.document=object.document;view.query=msi::prepare(document.data,msi_api::input(memory,args(1),wide));
            const auto id=bridge.add(std::move(view));memory.store(GuestAddress{args(2)},32,id);return done(0);
        }
        return done(120);
    }catch(const msi::Error& error){if(name=="MsiCreateRecord")return done(0);if(name=="MsiGetDatabaseState")return done(UINT32_MAX);if(name.starts_with("MsiDatabaseIsTablePersistent"))return done(3);return done(error.code);}
    catch(const compound::Error&){return done(1627);}catch(const MemoryFault&){return done(87);}catch(const std::invalid_argument&){return done(87);}catch(const std::bad_alloc&){return done(name=="MsiCreateRecord"?0:8);}
}
}
