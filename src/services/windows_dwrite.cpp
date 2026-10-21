#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "windows_locale_data.hpp"
#include "unicode_text_analysis.hpp"
#include "x64arm/native_font_data.hpp"
#include "x64arm/native_text_layout.hpp"
#include "x64arm/raster_surface.hpp"
#include <algorithm>
#include <set>
#include <tuple>
#include <bit>
#include <cmath>
#include <utility>

namespace x64arm {
namespace {
#include "dwrite_interfaces.hpp"
constexpr std::uint32_t invalid=0x80070057U, pointer_error=0x80004003U, no_interface=0x80004002U, insufficient=0x8007007aU;
struct FontBlob;
struct FontSet {
    // 集合保留建立時的來源身分；工廠更新快取不會改變已交付物件的語義。
    explicit FontSet(bool system=false):system_collection(system){}
    bool system_collection{};
    std::vector<NativeFontFace> faces;std::vector<std::u16string> families;
    // 私有集合保留原始檔案與 TTC face index；Blob 自己的 FontSet 不反向持有 Blob。
    std::vector<std::pair<std::shared_ptr<FontBlob>,std::size_t>> sources;
    std::vector<std::map<unsigned,std::vector<NativeFontName>>> properties;
    std::vector<unsigned> simulations;
};
struct FontBlob {
    std::vector<std::uint8_t> bytes,reference;std::uint64_t key{},loader{},modified{};
    std::shared_ptr<FontSet> fonts;std::u16string path;bool loaded{},native_export{};
    bool native_bytes_pending{};
    const std::vector<std::uint8_t>& contents(){
        if(native_bytes_pending){
            // 參考持有建立時的原生 face（含 variation／模擬），不重新依名稱查找。
            // 匯出失敗仍維持 pending，避免把空 bytes 當成成功的檔案快取。
            auto generated=native_font_file(fonts->faces.at(0));
            bytes=std::move(generated);native_bytes_pending=false;
        }
        return bytes;
    }
};
std::shared_ptr<FontBlob> native_font_blob(const NativeFontFace& face){
    auto blob=std::make_shared<FontBlob>();blob->fonts=std::make_shared<FontSet>();
    blob->fonts->faces={face};blob->fonts->families={face.family};
    // 已有可用 face 不代表已封裝完整 SFNT；GetFiles／GetReferenceKey 只建立參考。
    blob->loaded=true;blob->native_export=true;blob->native_bytes_pending=true;return blob;
}
FileSnapshot read_font_file(ReadOnlyFiles& filesystem,const std::string& path){
    // 字型的容量由共同契約限制；以一般唯讀 handle 保留 ACL／sharing 檢查，
    // 不受小型通用檔案快照快取的 16 MiB 容量限制。
    const auto source=filesystem.open_reader(path,{}, {},1);if(source.error)return {source.key,{},source.error};
    const auto information=source.file->information();if(information.error)return {source.key,{},information.error};
    if((information.metadata.attributes&16) || information.metadata.size>native_font_file_limit)return {source.key,{},8};
    auto bytes=std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(information.metadata.size));std::size_t offset=0;
    while(offset<bytes->size()){const auto read=source.file->read(std::span(*bytes).subspan(offset),offset);if(read.error)return {source.key,{},read.error};if(!read.bytes)return {source.key,{},1117};offset+=static_cast<std::size_t>(read.bytes);}
    std::array<std::uint8_t,1> extra{};const auto tail=source.file->read(extra,offset);if(tail.error || tail.bytes)return {source.key,{},tail.error?tail.error:1117};
    return {source.key,std::move(bytes),0};
}
#include "dwrite_font_tables.hpp"
struct FontMapping {
    std::vector<std::array<std::uint32_t,2>> ranges;std::vector<std::u16string> families;
    std::shared_ptr<FontSet> fonts;std::u16string locale,base;float scale{1};bool system{};
};
struct ColorLayer {NativeGlyphRun run;std::uint64_t face{};std::array<float,4> color{};std::uint16_t palette{};};
struct FallbackOperation {
    WindowsServiceDescriptor service;std::uint64_t scratch{},object{},source{};unsigned step{};
    std::u16string text,locale;
};
struct TextStyle {
    std::u16string family,locale;std::uint32_t weight{400},style{},stretch{5};float size{12};
    std::uint64_t collection{},effect{};bool underline{},strike{};
};
struct TextData {
    TextStyle format;NativeLayoutOptions options;std::uint32_t flow{},trimming[3]{};
    std::u16string input;std::map<std::uint32_t,TextStyle> ranges;
    std::optional<NativeTextLayout> layout;
};
struct TextDrawing {
    WindowsServiceDescriptor service;std::uint64_t scratch{},layout{};unsigned step{};std::size_t index{};
    std::vector<GuestCallback> callbacks;std::vector<std::uint64_t> faces;
};
enum class TextAnalysisStep { RetainSource,RetainSink,Text,Direction,Locale,Number,Report,NumberRelease };
struct TextAnalysisOperation {
    WindowsServiceDescriptor service;TextAnalysisStep step{};
    std::uint64_t scratch{},object{},source{},sink{},number{};
    unsigned start{},length{},position{},direction{},number_length{};std::size_t report_index{};
    bool source_retained{},sink_retained{};std::u16string text,locale;
    text_analysis::Analysis analysis;std::vector<std::uint8_t> breaks;
};
enum class FontOperationKind {Collection,Analyze,Face,Reference};
enum class FontOperationStep {RetainCollection,Enumerator,MoveNext,CurrentFile,Stream,Size,Fragment,ReleaseFragment,ReleaseStream,ReleaseFile};
struct FontOperation {
    WindowsServiceDescriptor service;FontOperationKind kind{};FontOperationStep step{};
    std::uint64_t scratch{},factory{},collection_loader{},enumerator{},file{},stream{},fragment{};
    std::uint64_t result{};std::uint32_t key_size{},files{};bool collection_retained{},fragment_open{};
    std::shared_ptr<FontSet> fonts;std::shared_ptr<FontBlob> blob;
    std::vector<std::uint8_t> collection_key;
};
struct Object {
    std::uint64_t target_dc{},target_bitmap{};std::array<std::uint32_t,2> target_size{};
    float target_scale{1};std::array<float,6> target_transform{1,0,0,1,0,0};
    std::shared_ptr<TextData> text_data;std::set<std::uint64_t> retained;
    std::vector<NativeFontName> localized;
    NativeGlyphRaster raster;unsigned rendering_mode{},pixel_geometry{1};
    std::array<float,3> blend{1.0f,0.0f,1.0f};
    float grayscale_contrast{};unsigned grid_fit{},simulations{};
    std::shared_ptr<FontBlob> blob;std::uint64_t reference_key{};std::size_t file_index{};
    unsigned number_method{};bool number_ignore_override{};
    Kind kind{};std::uint32_t references{1};std::shared_ptr<FontSet> fonts;
    std::vector<std::size_t> indices;std::u16string text;
    std::set<std::uint64_t> file_loaders,collection_loaders,font_tables;
    std::map<std::pair<std::uint64_t,std::vector<std::uint8_t>>,std::shared_ptr<FontSet>> collections;
    std::map<std::uint32_t,std::uint64_t> listeners;std::uint32_t next_listener{1};std::uint64_t generation{};
    std::vector<FontMapping> mappings;std::vector<ColorLayer> layers;std::size_t layer_index{};std::uint64_t current_layer{};
};
const Interface& info(Kind kind){return interfaces[static_cast<std::size_t>(kind)];}
Guid guid(const GuestMemory& memory,std::uint64_t at){Guid value{};memory.read(GuestAddress{at},value);return value;}
std::u16string folded(std::u16string value){for(auto& ch:value)if(ch>=u'A' && ch<=u'Z')ch+=32;return value;}
}
struct DWriteState {
    std::map<std::uint64_t,Object> objects;std::map<Kind,std::uint64_t> tables;
    std::map<std::uint64_t,std::weak_ptr<FontBlob>> files;std::uint64_t next_file{1},file_loader{};
    std::map<std::uint64_t,std::uint64_t> retained_loaders;
    std::shared_ptr<FontSet> system;std::uint64_t shared_factory{};
    std::map<std::uint64_t,FontOperation> operations;std::uint64_t next_operation{1},resuming{};
    std::vector<GuestCallback> deferred;
    std::map<std::uint64_t,TextDrawing> drawings;
    std::map<std::uint64_t,FallbackOperation> fallbacks;
    std::map<std::uint64_t,TextAnalysisOperation> analyses;
};
namespace {
GuestCallback font_callback(const GuestMemory& memory,unsigned unit,std::uint64_t object,unsigned slot,std::vector<std::uint64_t> arguments={},std::uint32_t wide=0){
    const auto bits=static_cast<std::uint8_t>(unit*8);const auto table=memory.load(GuestAddress{object},bits);
    arguments.insert(arguments.begin(),object);
    GuestCallback callback;callback.target=memory.load(GuestAddress{table+slot*unit},bits);callback.arguments=std::move(arguments);callback.wide_arguments=wide;return callback;
}
std::vector<GuestCallback> font_cleanup(const GuestMemory& memory,unsigned unit,const FontOperation& operation){
    std::vector<GuestCallback> callbacks;
    if(operation.fragment_open)callbacks.push_back(font_callback(memory,unit,operation.stream,4,{operation.fragment}));
    if(operation.stream)callbacks.push_back(font_callback(memory,unit,operation.stream,2));
    if(operation.file)callbacks.push_back(font_callback(memory,unit,operation.file,2));
    if(operation.enumerator)callbacks.push_back(font_callback(memory,unit,operation.enumerator,2));
    if(operation.collection_retained)callbacks.push_back(font_callback(memory,unit,operation.collection_loader,2));
    if(operation.factory)callbacks.push_back(font_callback(memory,unit,operation.factory,2));
    return callbacks;
}
unsigned font_face_type(FontBlob& blob){
    const auto& bytes=blob.contents();
    if(bytes.size()<4)return 6;
    if(std::equal(bytes.begin(),bytes.begin()+4,"ttcf"))return 2;
    return std::equal(bytes.begin(),bytes.begin()+4,"OTTO")?0:1;
}
void analyze_font(GuestMemory& memory,const WindowsCall& call,FontBlob& blob){
    const bool supported=blob.fonts && !blob.fonts->faces.empty();const auto type=font_face_type(blob);
    const std::array<std::uint32_t,4> values{supported?1U:0U,supported?(type==0?1U:type==1?2U:3U):0U,supported?type:6U,supported?static_cast<std::uint32_t>(blob.fonts->faces.size()):0U};
    for(unsigned i=0;i<4;++i)if(call(i+1))memory.store(GuestAddress{call(i+1)},32,values[i]);
}
}
std::optional<std::uint32_t> WindowsServices::resume_dwrite(GuestState& state,GuestMemory& memory,std::uint64_t token,std::uint64_t result){
    if(dwrite_ && dwrite_->analyses.contains(token)){
        dwrite_->resuming=token;state.reg(Gpr::Rax)=result;return invoke_dwrite(state,memory,dwrite_->analyses.at(token).service);
    }
    if(dwrite_ && dwrite_->fallbacks.contains(token)){
        dwrite_->resuming=token;state.reg(Gpr::Rax)=result;return invoke_dwrite(state,memory,dwrite_->fallbacks.at(token).service);
    }
    if(dwrite_ && dwrite_->drawings.contains(token)){
        dwrite_->resuming=token;state.reg(Gpr::Rax)=result;return invoke_dwrite(state,memory,dwrite_->drawings.at(token).service);
    }
    if(!dwrite_ || !dwrite_->operations.contains(token))throw HostCallFault("DirectWrite 回呼缺少字型載入狀態");
    auto& operation=dwrite_->operations.at(token);operation.result=result;const auto descriptor=operation.service;
    dwrite_->resuming=token;return invoke_dwrite(state,memory,descriptor);
}
void WindowsServices::discard_dwrite(std::uint64_t token,GuestMemory& memory,const GuestState& state){
    if(!dwrite_)return;
    if(auto at=dwrite_->analyses.find(token);at!=dwrite_->analyses.end()){
        const auto& operation=at->second;const auto unit=x86_?4U:8U;
        if(operation.number)dwrite_->deferred.push_back(font_callback(memory,unit,operation.number,2));
        if(operation.sink_retained)dwrite_->deferred.push_back(font_callback(memory,unit,operation.sink,2));
        if(operation.source_retained)dwrite_->deferred.push_back(font_callback(memory,unit,operation.source,2));
        dwrite_->deferred.push_back(font_callback(memory,unit,operation.object,2));
        (void)free_heap(memory,operation.scratch,state);dwrite_->analyses.erase(at);return;
    }
    if(auto at=dwrite_->fallbacks.find(token);at!=dwrite_->fallbacks.end()){
        dwrite_->deferred.push_back(font_callback(memory,x86_?4U:8U,at->second.object,2));
        dwrite_->deferred.push_back(font_callback(memory,x86_?4U:8U,at->second.source,2));
        (void)free_heap(memory,at->second.scratch,state);dwrite_->fallbacks.erase(at);return;
    }
    if(auto drawing=dwrite_->drawings.find(token);drawing!=dwrite_->drawings.end()){
        for(auto face:drawing->second.faces)dwrite_->deferred.push_back(font_callback(memory,x86_?4U:8U,face,2));
        dwrite_->deferred.push_back(font_callback(memory,x86_?4U:8U,drawing->second.layout,2));
        (void)free_heap(memory,drawing->second.scratch,state);dwrite_->drawings.erase(drawing);return;
    }
    const auto found=dwrite_->operations.find(token);if(found==dwrite_->operations.end())return;
    auto callbacks=font_cleanup(memory,x86_?4U:8U,found->second);
    dwrite_->deferred.insert(dwrite_->deferred.end(),std::make_move_iterator(callbacks.begin()),std::make_move_iterator(callbacks.end()));
    (void)free_heap(memory,found->second.scratch,state);dwrite_->operations.erase(found);
}
std::span<const WindowsServiceDescriptor> dwrite_service_descriptors(){
    static const auto table=[] {std::vector<WindowsServiceDescriptor> out{{"dwrite.dll","DWriteCreateFactory",3}};
        for(const auto& type:interfaces)out.insert(out.end(),type.methods.begin(),type.methods.end());return out;}();
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_dwrite(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const auto bits=static_cast<std::uint8_t>(x86_?32:64);const auto unit=x86_?4U:8U;
    const auto floating=[&](unsigned index){
        std::uint32_t raw=static_cast<std::uint32_t>(arg(index));
        if(!x86_ && index<4){raw=0;for(unsigned i=0;i<4;++i)raw|=static_cast<std::uint32_t>(state.xmm[index][i])<<(i*8);}
        return std::bit_cast<float>(raw);
    };
    const auto put=[&](std::uint64_t at,std::uint64_t value){memory.store(GuestAddress{at},bits,value);};
    const auto out=[&](unsigned n,std::uint64_t value){put(arg(n),value);};
    if(!runtime_.network.external_io)throw HostCallFault("DirectWrite 原生字型不支援 deterministic compare");
    if(!dwrite_)dwrite_=std::make_shared<DWriteState>();auto& bridge=*dwrite_;
    const auto finish=[&](std::uint64_t value)->std::optional<std::uint32_t>{
        // 最後一個 file/face/collection 消失後，才釋放外部 loader；回呼也可再次進入 DirectWrite。
        for(auto at=bridge.files.begin();at!=bridge.files.end();){
            if(!at->second.expired()){++at;continue;}
            const auto retained=bridge.retained_loaders.find(at->first);
            if(retained!=bridge.retained_loaders.end()){bridge.deferred.push_back(font_callback(memory,unit,retained->second,2));bridge.retained_loaders.erase(retained);}
            at=bridge.files.erase(at);
        }
        if(!bridge.deferred.empty()){auto callbacks=std::exchange(bridge.deferred,{});start_callbacks(state,memory,std::move(callbacks),Continuation::Return,value);}
        else arg.return_to(state,value);return std::nullopt;
    };
    const auto create=[&](Object object){
        if(bridge.objects.size()>=65536)throw std::bad_alloc{};
        const auto kind=object.kind;
        if((kind==Kind::Font || kind==Kind::FontFace || kind==Kind::FontFaceReference) && !object.blob && object.fonts && !object.indices.empty() && object.indices[0]<object.fonts->sources.size()){
            const auto& source=object.fonts->sources[object.indices[0]];object.blob=source.first;object.file_index=source.second;
        }
        if(!bridge.tables.contains(kind)){
            const auto& methods=info(kind).methods;const auto table=runtime_allocate(memory,{},methods.size()*unit);
            for(std::size_t i=0;i<methods.size();++i)put(table+i*unit,resolve(methods[i].family,methods[i].symbol).value);
            bridge.tables.emplace(kind,table);
        }
        const auto at=allocate_heap(memory,unit);if(!at)throw std::bad_alloc{};
        try{put(at,bridge.tables.at(kind));bridge.objects.emplace(at,std::move(object));}
        catch(...){(void)free_heap(memory,at,state);throw;}return at;
    };
    const auto system=[&]{
        if(!bridge.system){auto fonts=std::make_shared<FontSet>(true);fonts->faces=native_font_faces(u"");
            std::set<std::u16string> families;for(const auto& face:fonts->faces)families.insert(face.family);
            fonts->families.assign(families.begin(),families.end());bridge.system=std::move(fonts);}
        return bridge.system;
    };
    const auto strings=[&](std::u16string value){Object o;o.kind=Kind::LocalizedStrings;o.localized.push_back({u"en-us",std::move(value)});return create(std::move(o));};
    const auto end_operation=[&](std::uint64_t token,std::uint32_t status)->std::optional<std::uint32_t>{
        auto& operation=bridge.operations.at(token);auto callbacks=font_cleanup(memory,unit,operation);
        bridge.deferred.insert(bridge.deferred.end(),std::make_move_iterator(callbacks.begin()),std::make_move_iterator(callbacks.end()));
        (void)free_heap(memory,operation.scratch,state);bridge.operations.erase(token);return finish(status);
    };
    const auto next_operation=[&](std::uint64_t token,FontOperationStep step,std::uint64_t object,unsigned slot,std::vector<std::uint64_t> arguments={},std::uint32_t wide=0)->std::optional<std::uint32_t>{
        auto& operation=bridge.operations.at(token);operation.step=step;
        start_callbacks(state,memory,{font_callback(memory,unit,object,slot,std::move(arguments),wide)},Continuation::DWriteOperation,token);return std::nullopt;
    };
    const auto read_blob=[&](std::uint64_t token)->std::optional<std::uint32_t>{
        auto& operation=bridge.operations.at(token);const auto& blob=*operation.blob;
        memory.store(GuestAddress{operation.scratch},bits,0);
        if(!blob.reference.empty())memory.write(GuestAddress{operation.scratch+64},blob.reference);
        return next_operation(token,FontOperationStep::Stream,blob.loader,3,{operation.scratch+64,blob.reference.size(),operation.scratch});
    };
    const auto begin_file=[&](FontOperationKind kind,std::uint64_t file,std::uint64_t factory=0)->std::optional<std::uint32_t>{
        if(bridge.operations.size()>=128)return finish(0x8007000eU);
        FontOperation operation;operation.service=service;operation.kind=kind;operation.file=file;operation.factory=factory;operation.blob=bridge.objects.at(file).blob;
        operation.scratch=allocate_heap(memory,64+operation.blob->reference.size());if(!operation.scratch)return finish(0x8007000eU);
        ++bridge.objects.at(file).references;if(factory)++bridge.objects.at(factory).references;
        const auto token=bridge.next_operation++;bridge.operations.emplace(token,std::move(operation));return read_blob(token);
    };
    const auto resuming=std::exchange(bridge.resuming,0);
#include "dwrite_analysis_callbacks.inc"
    const auto cleanup_fallback=[&]{
        if(resuming && bridge.fallbacks.contains(resuming)){const auto operation=bridge.fallbacks.at(resuming);bridge.deferred.push_back(font_callback(memory,unit,operation.object,2));bridge.deferred.push_back(font_callback(memory,unit,operation.source,2));(void)free_heap(memory,operation.scratch,state);bridge.fallbacks.erase(resuming);}
    };
    if(resuming && bridge.drawings.contains(resuming)){
        auto& drawing=bridge.drawings.at(resuming);const auto status=static_cast<std::uint32_t>(state.reg(Gpr::Rax));
        const auto complete=[&](std::uint32_t result){
            for(auto face:drawing.faces)bridge.deferred.push_back(font_callback(memory,unit,face,2));
            bridge.deferred.push_back(font_callback(memory,unit,drawing.layout,2));
            (void)free_heap(memory,drawing.scratch,state);bridge.drawings.erase(resuming);return finish(result);
        };
        if(status&0x80000000U)return complete(status);
        if(drawing.step==0){drawing.step=1;if(memory.load(GuestAddress{drawing.scratch},32))drawing.index=3;else drawing.index=1;}
        else if(drawing.index<3)++drawing.index;
        else ++drawing.index;
        if(drawing.index==3 && !memory.load(GuestAddress{drawing.scratch},32)){
            const auto load_float=[&](unsigned offset){return std::bit_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{drawing.scratch+offset},32)));};
            const float a=load_float(4),b=load_float(8),c=load_float(12),d=load_float(16),dx=load_float(20),dy=load_float(24),scale=load_float(28),det=a*d-b*c;
            if(!std::isfinite(scale) || scale<=0)return complete(invalid);
            if(std::isfinite(det) && std::abs(det)>1e-12f)for(std::size_t i=3;i<drawing.callbacks.size();++i){
                auto& callback=drawing.callbacks[i];const float x=std::bit_cast<float>(static_cast<std::uint32_t>(callback.arguments[2])),y=std::bit_cast<float>(static_cast<std::uint32_t>(callback.arguments[3]));
                const float px=std::round((a*x+c*y+dx)*scale)/scale-dx,py=std::round((b*x+d*y+dy)*scale)/scale-dy;
                callback.arguments[2]=std::bit_cast<std::uint32_t>((d*px-c*py)/det);callback.arguments[3]=std::bit_cast<std::uint32_t>((a*py-b*px)/det);
            }
        }
        if(drawing.index>=drawing.callbacks.size())return complete(0);
        start_callbacks(state,memory,{drawing.callbacks[drawing.index]},Continuation::DWriteOperation,resuming);return std::nullopt;
    }
    if(resuming && bridge.operations.contains(resuming)){
        try{
            auto& operation=bridge.operations.at(resuming);const auto scratch=operation.scratch;
            const auto failed=static_cast<std::uint32_t>(operation.result)&0x80000000U;
            const auto move_next=[&]{memory.store(GuestAddress{scratch},32,0);return next_operation(resuming,FontOperationStep::MoveNext,operation.enumerator,3,{scratch});};
            const auto consume_blob=[&]()->std::optional<std::uint32_t>{
                const auto blob=operation.blob;
                if(operation.kind==FontOperationKind::Analyze){analyze_font(memory,arg,*blob);return end_operation(resuming,0);}
                if(operation.kind==FontOperationKind::Reference){
                    const auto index=arg(2);if(!blob->fonts || index>=blob->fonts->faces.size())return end_operation(resuming,0x88985000U);
                    Object value;value.kind=Kind::FontFaceReference;value.blob=blob;value.file_index=static_cast<std::size_t>(index);value.fonts=blob->fonts;value.indices={value.file_index};value.simulations=static_cast<unsigned>(arg(3));out(4,create(std::move(value)));return end_operation(resuming,0);
                }
                if(operation.kind==FontOperationKind::Face){
                    if(!blob->fonts || arg(4)>=blob->fonts->faces.size())return end_operation(resuming,0x88985000U);
                    const auto type=font_face_type(*blob);if(arg(1)!=type && arg(1)!=6)return end_operation(resuming,invalid);
                    Object value;value.kind=Kind::FontFace;value.blob=blob;value.file_index=static_cast<std::size_t>(arg(4));value.fonts=blob->fonts;value.indices={value.file_index};out(6,create(std::move(value)));return end_operation(resuming,0);
                }
                if(blob->fonts){
                    if(operation.fonts->faces.size()+blob->fonts->faces.size()>4096)return end_operation(resuming,0x8007000eU);
                    for(std::size_t i=0;i<blob->fonts->faces.size();++i){
                        const auto source=std::pair{blob,i};
                        if(std::find(operation.fonts->sources.begin(),operation.fonts->sources.end(),source)!=operation.fonts->sources.end())continue;
                        operation.fonts->faces.push_back(blob->fonts->faces[i]);operation.fonts->sources.push_back(source);
                    }
                }
                operation.blob.reset();const auto file=std::exchange(operation.file,0);
                return next_operation(resuming,FontOperationStep::ReleaseFile,file,2);
            };
            switch(operation.step){
            case FontOperationStep::RetainCollection:
                operation.collection_retained=true;put(scratch,0);
                return next_operation(resuming,FontOperationStep::Enumerator,operation.collection_loader,3,{operation.factory,scratch+64,operation.key_size,scratch});
            case FontOperationStep::Enumerator:
                if(failed)return end_operation(resuming,static_cast<std::uint32_t>(operation.result));
                operation.enumerator=memory.load(GuestAddress{scratch},bits);if(!operation.enumerator)return end_operation(resuming,pointer_error);return move_next();
            case FontOperationStep::MoveNext:
                if(failed)return end_operation(resuming,static_cast<std::uint32_t>(operation.result));
                if(!memory.load(GuestAddress{scratch},32)){
                    std::set<std::u16string> families;for(const auto& face:operation.fonts->faces)families.insert(face.family);
                    operation.fonts->families.assign(families.begin(),families.end());Object collection;collection.kind=Kind::FontCollection;collection.fonts=operation.fonts;
                    const auto key=std::pair{operation.collection_loader,operation.collection_key};
                    bridge.objects.at(operation.factory).collections.insert_or_assign(key,operation.fonts);
                    out(4,create(std::move(collection)));return end_operation(resuming,0);
                }
                if(++operation.files>4096)return end_operation(resuming,0x8007000eU);put(scratch,0);
                return next_operation(resuming,FontOperationStep::CurrentFile,operation.enumerator,4,{scratch});
            case FontOperationStep::CurrentFile:{
                if(failed)return end_operation(resuming,static_cast<std::uint32_t>(operation.result));
                operation.file=memory.load(GuestAddress{scratch},bits);const auto current=bridge.objects.find(operation.file);
                if(current==bridge.objects.end() || current->second.kind!=Kind::FontFile){operation.file=0;return end_operation(resuming,invalid);}
                operation.blob=current->second.blob;if(operation.blob->loaded)return consume_blob();
                // 自訂檔案 key 可大於集合 key；重新配置只影響本次 loader 操作。
                if(operation.blob->reference.size()>operation.key_size){const auto buffer=allocate_heap(memory,64+operation.blob->reference.size());if(!buffer)throw std::bad_alloc{};(void)free_heap(memory,operation.scratch,state);operation.scratch=buffer;}
                return read_blob(resuming);
            }
            case FontOperationStep::Stream:
                if(failed)return end_operation(resuming,static_cast<std::uint32_t>(operation.result));
                operation.stream=memory.load(GuestAddress{scratch},bits);if(!operation.stream)return end_operation(resuming,pointer_error);
                memory.store(GuestAddress{scratch+8},64,0);return next_operation(resuming,FontOperationStep::Size,operation.stream,5,{scratch+8});
            case FontOperationStep::Size:{
                if(failed)return end_operation(resuming,static_cast<std::uint32_t>(operation.result));
                const auto size=memory.load(GuestAddress{scratch+8},64);if(size>native_font_file_limit)return end_operation(resuming,0x8007000eU);
                std::size_t used=static_cast<std::size_t>(size);for(const auto& [key,file]:bridge.files){(void)key;if(const auto data=file.lock())used+=data->bytes.size();}
                if(used>native_font_collection_limit)return end_operation(resuming,0x8007000eU);
                put(scratch+16,0);put(scratch+24,0);
                return next_operation(resuming,FontOperationStep::Fragment,operation.stream,3,{scratch+16,0,size,scratch+24},12);
            }
            case FontOperationStep::Fragment:{
                if(failed)return end_operation(resuming,static_cast<std::uint32_t>(operation.result));
                const auto size=memory.load(GuestAddress{scratch+8},64),data=memory.load(GuestAddress{scratch+16},bits);
                operation.fragment=memory.load(GuestAddress{scratch+24},bits);operation.fragment_open=true;
                if(size && !data)return end_operation(resuming,pointer_error);
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));if(size)memory.read(GuestAddress{data},bytes);
                auto fonts=std::make_shared<FontSet>();fonts->faces=native_load_font_faces(bytes);
                std::set<std::u16string> families;for(const auto& face:fonts->faces)families.insert(face.family);fonts->families.assign(families.begin(),families.end());
                operation.blob->bytes=std::move(bytes);operation.blob->fonts=std::move(fonts);operation.blob->loaded=true;
                operation.fragment_open=false;return next_operation(resuming,FontOperationStep::ReleaseFragment,operation.stream,4,{operation.fragment});
            }
            case FontOperationStep::ReleaseFragment:{const auto stream=std::exchange(operation.stream,0);return next_operation(resuming,FontOperationStep::ReleaseStream,stream,2);}
            case FontOperationStep::ReleaseStream:return consume_blob();
            case FontOperationStep::ReleaseFile:return move_next();
            }
            throw HostCallFault("DirectWrite 字型載入階段無效");
        }catch(const std::bad_alloc&){return end_operation(resuming,0x8007000eU);}
    }
    try {
        if(name=="DWriteCreateFactory"){
            if(!arg(2))return finish(pointer_error);out(2,0);
            if(arg(0)>1 || !arg(1))return finish(invalid);
            const auto iid=guid(memory,arg(1));if(iid!=info(Kind::Factory).iid && iid!=unknown_iid && !supports(Kind::Factory,iid))return finish(no_interface);
            if(!arg(0) && bridge.shared_factory){++bridge.objects.at(bridge.shared_factory).references;out(2,bridge.shared_factory);}
            else {Object o;o.kind=Kind::Factory;const auto p=create(std::move(o));if(!arg(0))bridge.shared_factory=p;out(2,p);}
            return finish(0);
        }
        const auto found=bridge.objects.find(arg(0));if(found==bridge.objects.end())throw HostCallFault("無效或已釋放的 DirectWrite COM 介面");
        auto& o=found->second;const auto method=name.substr(name.find('.')+1);
        if(method=="QueryInterface"){
            if(!arg(2))return finish(pointer_error);out(2,0);if(!arg(1))return finish(pointer_error);
            const auto iid=guid(memory,arg(1));
            if(iid!=unknown_iid && iid!=info(o.kind).iid && !supports(o.kind,iid) && !(o.kind==Kind::TextLayout && iid==info(Kind::TextFormat).iid) && !(o.kind==Kind::FontFamily && iid==info(Kind::FontList).iid) && !(o.kind==Kind::LocalFontFileLoader && iid==info(Kind::FontFileLoader).iid))return finish(no_interface);
            ++o.references;out(2,arg(0));return finish(0);
        }
        if(method=="AddRef")return finish(++o.references);
        if(method=="Release"){
            if(--o.references)return finish(o.references);
            std::vector<GuestCallback> callbacks;
            for(const auto& [token,listener]:o.listeners){(void)token;callbacks.push_back(font_callback(memory,unit,listener,2));}
            for(auto retained:o.retained)callbacks.push_back(font_callback(memory,unit,retained,2));
            if(o.target_dc){gui_.contexts.erase(o.target_dc);gui_.dc_objects.erase(o.target_dc);gui_.saved_contexts.erase(o.target_dc);}
            if(o.target_bitmap){const auto bitmap=gui_.bitmaps.find(o.target_bitmap);if(bitmap!=gui_.bitmaps.end()){(void)free_heap(memory,bitmap->second.dib_address,state);gui_.bitmaps.erase(bitmap);}rasters_.erase(o.target_bitmap);raster_shared_.erase(o.target_bitmap);}
            for(const auto* list:{&o.file_loaders,&o.collection_loaders})for(auto loader:*list){const auto table=memory.load(GuestAddress{loader},bits);callbacks.push_back({memory.load(GuestAddress{table+2*unit},bits),{loader}});}
            for(const auto pointer:o.font_tables)(void)free_heap(memory,pointer,state);
            if(o.reference_key)(void)free_heap(memory,o.reference_key,state);
            if(bridge.file_loader==arg(0))bridge.file_loader=0;
            if(bridge.shared_factory==arg(0))bridge.shared_factory=0;
            const auto at=arg(0);bridge.objects.erase(found);(void)free_heap(memory,at,state);
            bridge.deferred.insert(bridge.deferred.end(),std::make_move_iterator(callbacks.begin()),std::make_move_iterator(callbacks.end()));return finish(0);
        }
        if(o.kind==Kind::Factory && method=="CreateNumberSubstitution"){
            if(!arg(4))return finish(pointer_error);out(4,0);
            if(arg(1)>4 || !arg(2))return finish(invalid);
            auto locale=read_utf16(memory,GuestAddress{arg(2)},85);
            if(!windows_locale_name_valid(locale))return finish(invalid);
            // IDWriteNumberSubstitution 是帶參數的 IUnknown；擁有 locale 複本，
            // 供文字分析／塑形使用，不借用呼叫者記憶體或改動共用 Factory。
            Object value;value.kind=Kind::NumberSubstitution;value.text=std::move(locale);
            value.number_method=static_cast<unsigned>(arg(1));value.number_ignore_override=arg(3)!=0;
            out(4,create(std::move(value)));return finish(0);
        }
        // 格式、範圍、排版、命中測試與 renderer 回呼共用原生字型資料。
#include "dwrite_text_analyzer.inc"
#include "dwrite_text_layout.inc"
#include "dwrite_bitmap_target.inc"
#include "dwrite_factory_extended.inc"
#include "dwrite_font_extended.inc"
#include "dwrite_fallback_extended.inc"
#include "dwrite_color_extended.inc"
        if(o.kind==Kind::RenderingParams){
            if(method=="GetPixelGeometry")return finish(o.pixel_geometry);
            if(method=="GetRenderingMode")return finish(o.rendering_mode==7?5:o.rendering_mode);
            if(method=="GetGamma" || method=="GetEnhancedContrast" || method=="GetClearTypeLevel" || method=="GetGrayscaleEnhancedContrast"){
                const auto value=method=="GetGrayscaleEnhancedContrast"?o.grayscale_contrast:o.blend[method=="GetGamma"?0:method=="GetEnhancedContrast"?1:2];
                const auto raw=std::bit_cast<std::uint32_t>(value);
                if(x86_){
                    const auto top=static_cast<unsigned>(((state.x87_status>>11)-1)&7);int exponent=0;const double fraction=std::frexp(value,&exponent);
                    state.x87[top]={static_cast<std::uint64_t>(fraction*0x1p64),static_cast<std::uint16_t>(value==0?0:exponent+16382)};
                    state.x87_status=static_cast<std::uint16_t>((state.x87_status&~0x3800U)|(top<<11));
                    state.x87_tag=static_cast<std::uint16_t>((state.x87_tag&~(3U<<(top*2)))|((value==0?1U:0U)<<(top*2)));
                }else {state.xmm[0]={};for(unsigned i=0;i<4;++i)state.xmm[0][i]=static_cast<std::uint8_t>(raw>>(i*8));}
                return finish(0);
            }
        }
        if(o.kind==Kind::GlyphRunAnalysis){
            if(method=="GetAlphaBlendParams"){
                const auto params=bridge.objects.find(arg(1));if(params==bridge.objects.end() || params->second.kind!=Kind::RenderingParams)return finish(invalid);
                for(unsigned i=2;i<5;++i){if(!arg(i))return finish(pointer_error);memory.check(GuestAddress{arg(i)},4,Access::Write);}
                // 原生 coverage 是線性的；GDI 模式採相同的預設線性混色，其餘遵循呼叫者參數。
                const auto blend=o.rendering_mode<=3?std::array<float,3>{1,0,1}:params->second.blend;
                for(unsigned i=0;i<3;++i)memory.store(GuestAddress{arg(i+2)},32,std::bit_cast<std::uint32_t>(blend[i]));return finish(0);
            }
            if(method=="GetAlphaTextureBounds" || method=="CreateAlphaTexture"){
                if(arg(1)>1)return finish(invalid);if(!arg(2))return finish(pointer_error);
                const bool matching=(arg(1)==0)==(o.raster.channels==1);
                if(method=="GetAlphaTextureBounds"){
                    const auto bounds=matching?o.raster.bounds:std::array<std::int32_t,4>{};memory.check(GuestAddress{arg(2)},16,Access::Write);
                    for(unsigned i=0;i<4;++i)memory.store(GuestAddress{arg(2)+i*4},32,static_cast<std::uint32_t>(bounds[i]));return finish(0);
                }
                std::array<std::int32_t,4> bounds{};for(unsigned i=0;i<4;++i)bounds[i]=static_cast<std::int32_t>(memory.load(GuestAddress{arg(2)+i*4},32));
                const auto width=static_cast<std::int64_t>(bounds[2])-bounds[0],height=static_cast<std::int64_t>(bounds[3])-bounds[1];const auto channels=arg(1)==0?1U:3U;
                if(width<0 || height<0 || width>32768 || height>32768)return finish(invalid);
                const auto size=static_cast<std::uint64_t>(width*height)*channels;if(size>arg(4))return finish(insufficient);if(size>64*1024*1024)return finish(0x8007000eU);
                if(size && !arg(3))return finish(pointer_error);if(size)memory.check(GuestAddress{arg(3)},static_cast<std::size_t>(size),Access::Write);
                std::vector<std::uint8_t> output(static_cast<std::size_t>(size));const auto& source=o.raster.bounds;
                if(matching && !o.raster.alpha.empty()){
                    const auto left=std::max(bounds[0],source[0]),top=std::max(bounds[1],source[1]),right=std::min(bounds[2],source[2]),bottom=std::min(bounds[3],source[3]);
                    if(right>left)for(auto y=top;y<bottom;++y){const auto from=(static_cast<std::size_t>(y-source[1])*static_cast<std::size_t>(source[2]-source[0])+static_cast<std::size_t>(left-source[0]))*channels;
                        const auto to=(static_cast<std::size_t>(y-bounds[1])*static_cast<std::size_t>(width)+static_cast<std::size_t>(left-bounds[0]))*channels;
                        std::copy_n(o.raster.alpha.begin()+static_cast<std::ptrdiff_t>(from),static_cast<std::size_t>(right-left)*channels,output.begin()+static_cast<std::ptrdiff_t>(to));}
                }
                if(size)memory.write(GuestAddress{arg(3)},output);return finish(0);
            }
        }
        if(o.kind==Kind::FontFile){
            if(method=="GetReferenceKey"){
                if(!arg(1) || !arg(2))return finish(pointer_error);memory.check(GuestAddress{arg(1)},unit,Access::Write);memory.check(GuestAddress{arg(2)},4,Access::Write);
                const auto size=o.blob->loader?o.blob->reference.size():8;
                if(!o.reference_key){o.reference_key=allocate_heap(memory,std::max<std::size_t>(1,size));if(!o.reference_key)throw std::bad_alloc{};if(o.blob->loader){if(size)memory.write(GuestAddress{o.reference_key},o.blob->reference);}else memory.store(GuestAddress{o.reference_key},64,o.blob->key);}
                out(1,o.reference_key);memory.store(GuestAddress{arg(2)},32,size);return finish(0);
            }
            if(method=="GetLoader"){
                if(!arg(1))return finish(pointer_error);memory.check(GuestAddress{arg(1)},unit,Access::Write);
                if(o.blob->loader){out(1,o.blob->loader);start_callbacks(state,memory,{font_callback(memory,unit,o.blob->loader,1)},Continuation::Return,0);return std::nullopt;}
                if(bridge.file_loader)++bridge.objects.at(bridge.file_loader).references;else {Object loader;loader.kind=Kind::LocalFontFileLoader;bridge.file_loader=create(std::move(loader));}out(1,bridge.file_loader);return finish(0);
            }
            if(method=="Analyze"){
                for(unsigned i=1;i<=4;++i){if(!arg(i)){if(i==3)continue;return finish(pointer_error);}memory.check(GuestAddress{arg(i)},4,Access::Write);}
                for(unsigned i=1;i<=4;++i)if(arg(i))memory.store(GuestAddress{arg(i)},32,i==3?6:0);
                if(!o.blob->loaded)return begin_file(FontOperationKind::Analyze,arg(0));
                analyze_font(memory,arg,*o.blob);return finish(0);
            }
        }
        if((o.kind==Kind::FontFileLoader || o.kind==Kind::LocalFontFileLoader) && method=="CreateStreamFromKey"){
            if(!arg(3))return finish(pointer_error);out(3,0);if(!arg(1) || arg(2)!=8)return finish(invalid);
            const auto key=memory.load(GuestAddress{arg(1)},64);const auto file=bridge.files.find(key);if(file==bridge.files.end())return finish(invalid);const auto data=file->second.lock();if(!data)return finish(invalid);
            (void)data->contents();
            Object stream;stream.kind=Kind::FontFileStream;stream.blob=data;out(3,create(std::move(stream)));return finish(0);
        }
        if(o.kind==Kind::LocalFontFileLoader){
            if(!arg(3))return finish(pointer_error);if(!arg(1) || arg(2)!=8)return finish(invalid);
            const auto found_file=bridge.files.find(memory.load(GuestAddress{arg(1)},64));if(found_file==bridge.files.end())return finish(invalid);
            const auto data=found_file->second.lock();if(!data || data->loader)return finish(invalid);
            if(data->path.empty() && data->native_export){
                const auto published=publish_font_file(data->contents());if(published.error)return finish(0x80070000U|published.error);
                data->path=utf8_to_utf16(published.key);std::replace(data->path.begin(),data->path.end(),u'/',u'\\');data->modified=published.metadata.modified;
            }
            if(data->path.empty())return finish(invalid);
            if(method=="GetFilePathLengthFromKey"){memory.store(GuestAddress{arg(3)},32,data->path.size());return finish(0);}
            if(method=="GetFilePathFromKey"){
                if(arg(4)<=data->path.size())return finish(insufficient);memory.write(GuestAddress{arg(3)},utf16_bytes(data->path));return finish(0);
            }
            if(method=="GetLastWriteTimeFromKey"){memory.store(GuestAddress{arg(3)},64,data->modified);return finish(0);}
        }
        if(o.kind==Kind::FontFileStream){
            if(method=="GetFileSize" || method=="GetLastWriteTime"){
                if(!arg(1))return finish(pointer_error);memory.store(GuestAddress{arg(1)},64,method=="GetFileSize"?o.blob->contents().size():o.blob->modified);return finish(method=="GetFileSize" || o.blob->modified?0:0x80004001U);
            }
            if(method=="ReadFileFragment"){
                if(!arg(1) || !arg(4))return finish(pointer_error);memory.check(GuestAddress{arg(1)},unit,Access::Write);memory.check(GuestAddress{arg(4)},unit,Access::Write);out(1,0);out(4,0);
                const auto& bytes=o.blob->contents();
                const auto offset=arg(2),size=arg(3);if(offset>bytes.size() || size>bytes.size()-offset)return finish(invalid);
                const auto buffer=allocate_heap(memory,std::max<std::uint64_t>(1,size));if(!buffer)throw std::bad_alloc{};
                try{o.font_tables.insert(buffer);if(size)memory.write(GuestAddress{buffer},std::span(bytes).subspan(static_cast<std::size_t>(offset),static_cast<std::size_t>(size)));}catch(...){o.font_tables.erase(buffer);(void)free_heap(memory,buffer,state);throw;}
                out(1,buffer);out(4,buffer);return finish(0);
            }
            if(method=="ReleaseFileFragment"){if(arg(1)){if(!o.font_tables.erase(arg(1)))throw HostCallFault("無效的 DirectWrite 檔案 fragment context");(void)free_heap(memory,arg(1),state);}return finish(0);}
        }
        if(o.kind==Kind::Factory){
            if(method=="CreateRenderingParams" || method=="CreateMonitorRenderingParams" || method=="CreateCustomRenderingParams"){
                const bool custom=method=="CreateCustomRenderingParams";const auto output=custom?6U:method=="CreateMonitorRenderingParams"?2U:1U;if(!arg(output))return finish(pointer_error);out(output,0);
                Object value;value.kind=Kind::RenderingParams;
                if(!custom){value.rendering_mode=gui_.font_smoothing?0:1;value.pixel_geometry=gui_.font_smoothing_type==2?(gui_.font_smoothing_orientation?1U:2U):0;value.blend[2]=gui_.font_smoothing && gui_.font_smoothing_type==2?1.0F:0.0F;}
                if(custom){value.blend={floating(1),floating(2),floating(3)};value.pixel_geometry=static_cast<unsigned>(arg(4));value.rendering_mode=static_cast<unsigned>(arg(5));
                    if(std::any_of(value.blend.begin(),value.blend.end(),[](float v){return !std::isfinite(v);}) || value.blend[0]<=0 || value.blend[1]<0 || value.blend[2]<0 || value.blend[2]>1 || value.pixel_geometry>2 || value.rendering_mode>6)return finish(invalid);}
                out(output,create(std::move(value)));return finish(0);
            }
            if(method=="CreateGlyphRunAnalysis" || method=="CreateGlyphRunAnalysis2" || method=="CreateGlyphRunAnalysis3"){
                const bool extended=method!="CreateGlyphRunAnalysis";const unsigned output=extended?9:8,render_index=extended?3:4,measure_index=extended?4:5,transform_index=extended?2:3;
                if(!arg(output))return finish(pointer_error);out(output,0);if(!arg(1))return finish(pointer_error);
                if(arg(render_index)<1 || arg(render_index)>(method=="CreateGlyphRunAnalysis3"?7U:6U) || arg(render_index)==6 || arg(measure_index)>2 || (extended && (arg(5)>2 || arg(6)>1)))return finish(invalid);
                const auto run_at=arg(1);memory.check(GuestAddress{run_at},x86_?32:48,Access::Read);
                const auto source=bridge.objects.find(memory.load(GuestAddress{run_at},bits));if(source==bridge.objects.end() || source->second.kind!=Kind::FontFace)return finish(invalid);
                const auto face=effective_face(source->second.fonts->faces.at(source->second.indices.at(0)),source->second.simulations);
                const auto read_float=[&](std::uint64_t at){return std::bit_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{at},32)));};
                NativeGlyphRun run;run.em=read_float(run_at+unit);run.pixels_per_dip=extended?1:floating(2);run.baseline_x=floating(extended?7:6);run.baseline_y=floating(extended?8:7);run.aliased=arg(render_index)==1;run.measuring=static_cast<unsigned>(arg(measure_index));
                if(!std::isfinite(run.em) || run.em<=0 || !std::isfinite(run.pixels_per_dip) || run.pixels_per_dip<=0 || !std::isfinite(run.baseline_x) || !std::isfinite(run.baseline_y))return finish(invalid);
                if(arg(transform_index))for(unsigned i=0;i<6;++i){run.transform[i]=read_float(arg(transform_index)+i*4);if(!std::isfinite(run.transform[i]))return finish(invalid);}
                const auto count=memory.load(GuestAddress{run_at+unit+4},32);if(count>65536)return finish(invalid);
                const auto arrays=run_at+unit+8,indices=memory.load(GuestAddress{arrays},bits),advances=memory.load(GuestAddress{arrays+unit},bits),offsets=memory.load(GuestAddress{arrays+2*unit},bits);
                run.sideways=memory.load(GuestAddress{arrays+3*unit},32)!=0;run.rtl=(memory.load(GuestAddress{arrays+3*unit+4},32)&1)!=0;
                if(count && !indices)return finish(pointer_error);run.indices.resize(static_cast<std::size_t>(count));if(advances)run.advances.resize(static_cast<std::size_t>(count));if(offsets)run.offsets.resize(static_cast<std::size_t>(count));
                for(unsigned i=0;i<count;++i){run.indices[i]=static_cast<std::uint16_t>(memory.load(GuestAddress{indices+i*2},16));
                    if(advances){run.advances[i]=read_float(advances+i*4);if(!std::isfinite(run.advances[i]))return finish(invalid);}
                    if(offsets)for(unsigned j=0;j<2;++j){run.offsets[i][j]=read_float(offsets+i*8+j*4);if(!std::isfinite(run.offsets[i][j]))return finish(invalid);}}
                Object value;value.kind=Kind::GlyphRunAnalysis;value.rendering_mode=static_cast<unsigned>(arg(render_index));
                try{value.raster=native_font_glyph_raster(face,run);}catch(const std::invalid_argument&){return finish(invalid);}
                if(extended && arg(6)==1 && value.raster.channels==3){std::vector<std::uint8_t> gray(value.raster.alpha.size()/3);for(std::size_t i=0;i<gray.size();++i)gray[i]=static_cast<std::uint8_t>((static_cast<unsigned>(value.raster.alpha[i*3])+value.raster.alpha[i*3+1]+value.raster.alpha[i*3+2]+1)/3);value.raster.alpha=std::move(gray);value.raster.channels=1;}
                std::size_t used=value.raster.alpha.size();for(const auto& [handle,object]:bridge.objects){(void)handle;used+=object.raster.alpha.size();if(used>64*1024*1024)throw std::bad_alloc{};}
                out(output,create(std::move(value)));return finish(0);
            }
            if(method=="CreateFontFace"){
                if(!arg(6))return finish(pointer_error);out(6,0);if(arg(2)!=1 || !arg(3) || arg(1)>6)return finish(invalid);if(arg(5))return finish(0x80004001U);
                const auto file_at=memory.load(GuestAddress{arg(3)},bits);const auto file=bridge.objects.find(file_at);if(file==bridge.objects.end() || file->second.kind!=Kind::FontFile || !file->second.blob)return finish(invalid);
                const auto data=file->second.blob;if(!data->loaded)return begin_file(FontOperationKind::Face,file_at,arg(0));
                if(!data->fonts || arg(4)>=data->fonts->faces.size())return finish(0x88985000U);
                const auto type=font_face_type(*data);if(arg(1)!=type && arg(1)!=6)return finish(invalid);
                Object value;value.kind=Kind::FontFace;value.blob=data;value.file_index=static_cast<std::size_t>(arg(4));value.fonts=data->fonts;value.indices={value.file_index};out(6,create(std::move(value)));return finish(0);
            }
            if(method=="CreateCustomFontFileReference"){
                if(!arg(4))return finish(pointer_error);out(4,0);if((arg(2) && !arg(1)) || !arg(3) || arg(2)>65536)return finish(invalid);
                const auto loader=bridge.objects.find(arg(3));std::shared_ptr<FontBlob> data;
                if(loader!=bridge.objects.end() && (loader->second.kind==Kind::FontFileLoader || loader->second.kind==Kind::LocalFontFileLoader)){
                    if(arg(2)!=8)return finish(invalid);const auto file=bridge.files.find(memory.load(GuestAddress{arg(1)},64));if(file==bridge.files.end())return finish(invalid);data=file->second.lock();if(!data)return finish(invalid);
                }else{
                    if(!o.file_loaders.contains(arg(3)))return finish(invalid);
                    std::vector<std::uint8_t> reference(static_cast<std::size_t>(arg(2)));if(arg(2))memory.read(GuestAddress{arg(1)},reference);
                    for(const auto& [key,file]:bridge.files){(void)key;if(const auto existing=file.lock();existing && existing->loader==arg(3) && existing->reference==reference){data=existing;break;}}
                    if(!data){
                        data=std::make_shared<FontBlob>();data->loader=arg(3);data->reference=std::move(reference);data->key=bridge.next_file++;
                        bridge.files.emplace(data->key,data);
                        Object value;value.kind=Kind::FontFile;value.blob=data;out(4,create(std::move(value)));
                        bridge.retained_loaders.emplace(data->key,data->loader);
                        // 建立 reference 不讀 stream；Analyze／CreateFontFace／collection 列舉才載入。
                        start_callbacks(state,memory,{font_callback(memory,unit,data->loader,1)},Continuation::Return,0);return std::nullopt;
                    }
                }
                Object value;value.kind=Kind::FontFile;value.blob=data;out(4,create(std::move(value)));return finish(0);
            }
            if(method=="CreateFontFileReference"){
                if(!arg(3))return finish(pointer_error);out(3,0);if(!arg(1))return finish(invalid);
                const auto path=utf16_to_utf8(read_utf16(memory,GuestAddress{arg(1)},32768));const auto metadata=filesystem_.information(path);
                if(metadata.error)return finish(0x80070000U|metadata.error);if(metadata.metadata.attributes&16)return finish(invalid);if(metadata.metadata.size>native_font_file_limit)return finish(0x8007000eU);
                const auto source=read_font_file(filesystem_,path);if(source.error)return finish(0x80070000U|source.error);
                auto data=std::make_shared<FontBlob>();data->bytes=*source.bytes;data->fonts=std::make_shared<FontSet>();data->fonts->faces=native_load_font_faces(data->bytes);data->loaded=true;data->key=bridge.next_file++;
                data->path=utf8_to_utf16(source.key);std::replace(data->path.begin(),data->path.end(),u'/',u'\\');
                data->modified=arg(2)?memory.load(GuestAddress{arg(2)},64):metadata.metadata.modified;
                std::set<std::u16string> families;for(const auto& face:data->fonts->faces)families.insert(face.family);data->fonts->families.assign(families.begin(),families.end());
                bridge.files.emplace(data->key,data);Object value;value.kind=Kind::FontFile;value.blob=data;out(3,create(std::move(value)));return finish(0);
            }
            if(method=="CreateCustomFontCollection"){
                if(!arg(4))return finish(pointer_error);out(4,0);
                if(!arg(1) || !o.collection_loaders.contains(arg(1)) || (arg(3) && !arg(2)) || arg(3)>65536)return finish(invalid);
                std::vector<std::uint8_t> key(static_cast<std::size_t>(arg(3)));if(arg(3))memory.read(GuestAddress{arg(2)},key);
                const auto cached=o.collections.find(std::pair{arg(1),key});
                if(cached!=o.collections.end()){Object collection;collection.kind=Kind::FontCollection;collection.fonts=cached->second;out(4,create(std::move(collection)));return finish(0);}
                if(bridge.operations.size()>=128)return finish(0x8007000eU);
                FontOperation operation;operation.service=service;operation.kind=FontOperationKind::Collection;operation.factory=arg(0);operation.collection_loader=arg(1);operation.key_size=static_cast<std::uint32_t>(arg(3));operation.fonts=std::make_shared<FontSet>();
                operation.collection_key=std::move(key);
                operation.scratch=allocate_heap(memory,64+operation.key_size);if(!operation.scratch)throw std::bad_alloc{};
                if(operation.key_size)memory.write(GuestAddress{operation.scratch+64},operation.collection_key);
                ++o.references;const auto token=bridge.next_operation++;bridge.operations.emplace(token,std::move(operation));
                return next_operation(token,FontOperationStep::RetainCollection,arg(1),1);
            }
            if(method=="GetSystemFontCollection"){
                if(!arg(1))return finish(pointer_error);out(1,0);if(arg(2))bridge.system.reset();Object collection;collection.kind=Kind::FontCollection;collection.fonts=system();out(1,create(std::move(collection)));return finish(0);
            }
            if(method=="RegisterFontFileLoader" || method=="UnregisterFontFileLoader" || method=="RegisterFontCollectionLoader" || method=="UnregisterFontCollectionLoader"){
                if(!arg(1))return finish(invalid);auto& list=method.ends_with("FileLoader")?o.file_loaders:o.collection_loaders;
                const bool add=method.starts_with("Register");if(add==list.contains(arg(1)))return finish(invalid);
                const auto table=memory.load(GuestAddress{arg(1)},bits),callback=memory.load(GuestAddress{table+(add?1:2)*unit},bits);
                if(add)list.insert(arg(1));else {list.erase(arg(1));if(method.ends_with("CollectionLoader"))std::erase_if(o.collections,[&](const auto& item){return item.first.first==arg(1);});}
                bridge.deferred.push_back({callback,{arg(1)}});return finish(0);
            }
            if(method=="GetGdiInterop"){if(!arg(1))return finish(pointer_error);Object value;value.kind=Kind::GdiInterop;value.fonts=system();out(1,create(std::move(value)));return finish(0);}
        }
        if(o.kind==Kind::GdiInterop && (method=="ConvertFontToLOGFONT" || method=="ConvertFontFaceToLOGFONT")){
            const bool font=method=="ConvertFontToLOGFONT";
            if(!arg(2) || (font && !arg(3)))return finish(pointer_error);
            const auto source=bridge.objects.find(arg(1));
            if(source==bridge.objects.end() || source->second.kind!=(font?Kind::Font:Kind::FontFace))return finish(invalid);
            const auto face=effective_face(source->second.fonts->faces.at(source->second.indices.at(0)),source->second.simulations);
            memory.check(GuestAddress{arg(2)},92,Access::Write);if(font)memory.check(GuestAddress{arg(3)},4,Access::Write);
            memory.write(GuestAddress{arg(2)},std::array<std::uint8_t,92>{});
            memory.store(GuestAddress{arg(2)+16},32,static_cast<std::uint32_t>(face.weight));memory.store(GuestAddress{arg(2)+20},8,face.italic);
            memory.store(GuestAddress{arg(2)+23},8,(face.signature[4]&0x80000000U)?2:1);
            memory.store(GuestAddress{arg(2)+24},8,3);memory.store(GuestAddress{arg(2)+27},8,face.family_class|(face.fixed_pitch?1:2));
            auto count=std::min<std::size_t>(31,face.family.size());if(count && face.family[count-1]>=0xd800 && face.family[count-1]<=0xdbff)--count;
            memory.write(GuestAddress{arg(2)+28},utf16_bytes(face.family.substr(0,count)));
            if(font)memory.store(GuestAddress{arg(3)},32,source->second.fonts->system_collection);return finish(0);
        }
        if(o.kind==Kind::GdiInterop && method=="CreateFontFromLOGFONT"){
            if(!arg(2))return finish(pointer_error);out(2,0);if(!arg(1))return finish(invalid);
            memory.check(GuestAddress{arg(1)},92,Access::Read);
            std::u16string family;for(unsigned i=0;i<32;++i){const auto ch=static_cast<char16_t>(memory.load(GuestAddress{arg(1)+28+i*2},16));if(!ch)break;family.push_back(ch);}
            const auto query=folded(o.fonts->system_collection && windows_ui_font_alias(family)?native_ui_font_family():family);const auto requested=static_cast<std::int32_t>(memory.load(GuestAddress{arg(1)+16},32));
            const auto weight=requested==0?400:std::clamp(requested,1,999);const bool italic=memory.load(GuestAddress{arg(1)+20},8)!=0;
            std::optional<std::size_t> best;
            const auto rank=[&](std::size_t i){const auto& f=o.fonts->faces[i];return std::tuple{f.italic!=italic,std::abs(f.weight-weight)};};
            for(std::size_t i=0;i<o.fonts->faces.size();++i){const auto& f=o.fonts->faces[i];if(folded(f.family)!=query && folded(f.full_name)!=query)continue;if(!best || rank(i)<rank(*best))best=i;}
            if(!best)return finish(0x88985002U); // DWRITE_E_NOFONT：沒有捏造不存在的字型。
            Object value;value.kind=Kind::Font;value.fonts=o.fonts;value.indices={*best};out(2,create(std::move(value)));return finish(0);
        }
        if(o.kind==Kind::GdiInterop && method=="CreateFontFaceFromHdc"){
            if(!arg(2))return finish(pointer_error);out(2,0);
            const auto dc=gui_.contexts.find(arg(1));if(dc==gui_.contexts.end())return finish(invalid);
            const auto face=native_resolve_font(gui_font(gui_,dc->second.font));if(!face)return finish(0x88985002U);
            auto& faces=o.fonts->faces;auto it=std::find_if(faces.begin(),faces.end(),[&](const auto& f){return f.postscript_name==face->postscript_name;});
            std::size_t index=static_cast<std::size_t>(it-faces.begin());
            if(it==faces.end()){
                faces.push_back(*face);auto& families=o.fonts->families;
                if(std::find(families.begin(),families.end(),face->family)==families.end())families.push_back(face->family);
            }
            Object value;value.kind=Kind::FontFace;value.fonts=o.fonts;value.indices={index};out(2,create(std::move(value)));return finish(0);
        }
        if(o.kind==Kind::FontCollection){
            if(method=="GetFontFromFontFace"){
                if(!arg(2))return finish(pointer_error);out(2,0);const auto other=bridge.objects.find(arg(1));
                if(other==bridge.objects.end() || other->second.kind!=Kind::FontFace)return finish(invalid);
                const auto& source=other->second;const auto& face=source.fonts->faces.at(source.indices.at(0));
                const auto it=std::find_if(o.fonts->faces.begin(),o.fonts->faces.end(),[&](const auto& f){
                    const auto index=static_cast<std::size_t>(&f-o.fonts->faces.data());
                    if(index<o.fonts->sources.size())return o.fonts->sources[index]==std::pair{source.blob,source.file_index};
                    return f.postscript_name==face.postscript_name;
                });
                if(it==o.fonts->faces.end())return finish(0x88985002U);
                Object value;value.kind=Kind::Font;value.fonts=o.fonts;value.indices={static_cast<std::size_t>(it-o.fonts->faces.begin())};out(2,create(std::move(value)));return finish(0);
            }
            if(method=="GetFontFamilyCount")return finish(o.fonts->families.size());
            if(method=="FindFamilyName"){
                if(!arg(1)||!arg(2)||!arg(3))return finish(pointer_error);const auto requested=read_utf16(memory,GuestAddress{arg(1)},1024);
                const auto query=folded(o.fonts->system_collection && windows_ui_font_alias(requested)?native_ui_font_family():requested);
                const auto& families=o.fonts->families;const auto i=std::find_if(families.begin(),families.end(),[&](const auto& f){return folded(f)==query;});
                memory.store(GuestAddress{arg(2)},32,i==families.end()?UINT32_MAX:static_cast<std::uint32_t>(i-families.begin()));memory.store(GuestAddress{arg(3)},32,i!=families.end());return finish(0);
            }
            if(method=="GetFontFamily" || method=="GetFontFamily1"){
                if(!arg(2))return finish(pointer_error);out(2,0);if(arg(1)>=o.fonts->families.size())return finish(invalid);
                Object family;family.kind=Kind::FontFamily;family.fonts=o.fonts;family.text=o.fonts->families[arg(1)];
                for(std::size_t i=0;i<o.fonts->faces.size();++i)if(o.fonts->faces[i].family==family.text)family.indices.push_back(i);
                out(2,create(std::move(family)));return finish(0);
            }
        }
        if(o.kind==Kind::FontFamily || o.kind==Kind::FontList){
            if(method=="GetFontCount")return finish(o.indices.size());
            if(method=="GetFontCollection"){if(!arg(1))return finish(pointer_error);Object value;value.kind=Kind::FontCollection;value.fonts=o.fonts;out(1,create(std::move(value)));return finish(0);}
            if(method=="GetFamilyNames"){if(!arg(1))return finish(pointer_error);out(1,strings(o.text));return finish(0);}
            if(method=="GetFont" || method=="GetFont1"){
                if(!arg(2))return finish(pointer_error);out(2,0);if(arg(1)>=o.indices.size())return finish(invalid);
                Object value;value.kind=Kind::Font;value.fonts=o.fonts;value.indices={o.indices[arg(1)]};out(2,create(std::move(value)));return finish(0);
            }
            if(method=="GetFirstMatchingFont" || method=="GetMatchingFonts"){
                if(!arg(4))return finish(pointer_error);out(4,0);if(arg(1)<1||arg(1)>999||arg(2)<1||arg(2)>9||arg(3)>2)return finish(invalid);
                auto indices=o.indices;std::stable_sort(indices.begin(),indices.end(),[&](auto a,auto b){
                    const auto rank=[&](auto i){const auto& f=o.fonts->faces[i];return std::tuple{f.italic!=(arg(3)!=0),std::abs(f.weight-static_cast<int>(arg(1)))};};return rank(a)<rank(b);});
                if(indices.empty())return finish(0x88985002U);Object value;value.kind=method=="GetMatchingFonts"?Kind::FontList:Kind::Font;value.fonts=o.fonts;
                if(value.kind==Kind::Font)indices.resize(1);value.indices=std::move(indices);out(4,create(std::move(value)));return finish(0);
            }
        }
        if(o.kind==Kind::Font || o.kind==Kind::FontFace){
            const auto face=effective_face(o.fonts->faces.at(o.indices.at(0)),o.simulations);
            if(method=="GetIndex")return finish(o.file_index);
            if(method=="GetType")return finish(o.blob?font_face_type(*o.blob):!native_font_table(face,0x43464620U).empty() || !native_font_table(face,0x43464632U).empty()?0:1);
            if(method=="GetFiles"){
                if(!arg(1))return finish(pointer_error);memory.check(GuestAddress{arg(1)},4,Access::Write);const auto capacity=memory.load(GuestAddress{arg(1)},32);memory.store(GuestAddress{arg(1)},32,1);
                if(!arg(2))return finish(0);if(capacity<1)return finish(insufficient);memory.check(GuestAddress{arg(2)},unit,Access::Write);out(2,0);
                if(!o.blob){auto data=native_font_blob(face);data->key=bridge.next_file++;bridge.files.emplace(data->key,data);o.blob=std::move(data);}
                Object file;file.kind=Kind::FontFile;file.blob=o.blob;out(2,create(std::move(file)));return finish(0);
            }
            if(method=="TryGetFontTable"){
                if(!arg(2)||!arg(3)||!arg(4)||!arg(5))return finish(pointer_error);
                for(unsigned i=2;i<=5;++i)memory.check(GuestAddress{arg(i)},(i==2 || i==4)?unit:4,Access::Write);
                out(2,0);memory.store(GuestAddress{arg(3)},32,0);out(4,0);memory.store(GuestAddress{arg(5)},32,0);
                const auto tag=static_cast<std::uint32_t>(arg(1));const auto native_tag=(tag>>24)|((tag>>8)&0xff00U)|((tag<<8)&0xff0000U)|(tag<<24);
                const auto data=native_font_table(face,native_tag);if(data.empty())return finish(0);
                if(data.size()>UINT32_MAX)return finish(0x8007000eU);const auto pointer=allocate_heap(memory,data.size());if(!pointer)return finish(0x8007000eU);
                try{o.font_tables.insert(pointer);memory.write(GuestAddress{pointer},data);}catch(...){o.font_tables.erase(pointer);(void)free_heap(memory,pointer,state);throw;}
                out(2,pointer);memory.store(GuestAddress{arg(3)},32,data.size());out(4,pointer);memory.store(GuestAddress{arg(5)},32,1);return finish(0);
            }
            if(method=="ReleaseFontTable"){
                if(arg(1)){if(!o.font_tables.erase(arg(1)))throw HostCallFault("無效的 DirectWrite 字型資料 context");(void)free_heap(memory,arg(1),state);}return finish(0);
            }
            if(method=="GetGlyphIndices" || method=="HasCharacter"){
                const bool single=method=="HasCharacter";const auto count=single?1U:static_cast<std::uint32_t>(arg(2));const auto output=arg(single?2:3);
                if(count>1048576)return finish(invalid);if(count && (!output || (!single && !arg(1))))return finish(pointer_error);
                if(count)memory.check(GuestAddress{output},count*(single?4U:2U),Access::Write);
                std::vector<std::uint32_t> input(count);for(unsigned i=0;i<count;++i)input[i]=static_cast<std::uint32_t>(single?arg(1):memory.load(GuestAddress{arg(1)+i*4},32));
                const auto indices=native_font_glyph_indices(face,input);for(unsigned i=0;i<count;++i)memory.store(GuestAddress{output+i*(single?4U:2U)},single?32:16,single?indices[i]!=0:indices[i]);return finish(0);
            }
            if(method=="GetGlyphCount"){
                const auto data=native_font_table(face,0x6d617870U);return finish(data.size()>=6?(static_cast<unsigned>(data[4])<<8)|data[5]:0);
            }
            if(method=="GetGlyphRunOutline"){
                const auto em=floating(1);const auto count=static_cast<std::uint32_t>(arg(5));const bool sideways=arg(6)!=0,rtl=arg(7)!=0;
                if(!std::isfinite(em) || em<=0 || count>1048576 || (sideways && rtl))return finish(invalid);
                if(!arg(8) || (count && !arg(2)))return finish(pointer_error);
                if(!count)return finish(0);
                memory.check(GuestAddress{arg(2)},count*2U,Access::Read);
                if(arg(3))memory.check(GuestAddress{arg(3)},count*4U,Access::Read);
                if(arg(4))memory.check(GuestAddress{arg(4)},count*8U,Access::Read);
                std::vector<std::uint16_t> indices(count);for(unsigned i=0;i<count;++i)indices[i]=static_cast<std::uint16_t>(memory.load(GuestAddress{arg(2)+i*2},16));
                // 輪廓使用同一個真實 CoreText face，包括私有 SFNT／TTC；不以字型名稱另行選字。
                std::vector<NativeGlyphMetrics> metrics;try{metrics=native_font_glyph_metrics(face,indices);}catch(const std::invalid_argument&){return finish(invalid);}
                const double scale=static_cast<double>(em)/face.em;
                const auto read_float=[&](std::uint64_t at){return std::bit_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{at},32)));};
                struct Command{unsigned slot;std::uint64_t point;std::size_t offset,count;};
                std::vector<Command> commands{{3,0,0,1},{4,0,0,0}};std::vector<std::uint8_t> data;double cursor=0;
                const auto point_bits=[](const std::array<double,2>& point){
                    const auto x=static_cast<float>(point[0]),y=static_cast<float>(point[1]);
                    if(!std::isfinite(x) || !std::isfinite(y))throw std::invalid_argument("字形輪廓座標溢位");
                    return static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(x))|(static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(y))<<32);
                };
                const auto append=[&](std::uint64_t point){if(data.size()>output_limit-8)throw std::bad_alloc{};for(unsigned j=0;j<8;++j)data.push_back(static_cast<std::uint8_t>(point>>(j*8)));};
                try{for(unsigned i=0;i<count;++i){
                    const double advance=arg(3)?read_float(arg(3)+i*4):metrics[i][sideways?4:1]*scale;
                    const double offset=arg(4)?read_float(arg(4)+i*8):0,ascender=arg(4)?read_float(arg(4)+i*8+4):0;
                    if(!std::isfinite(advance) || !std::isfinite(offset) || !std::isfinite(ascender))return finish(invalid);
                    if(rtl)cursor-=advance;const double x=cursor+(rtl?-offset:offset),y=-ascender;
                    const auto glyph=native_gdi_glyph(face,indices[i],{scale,0,0,-scale},0);
                    const auto position=[&](const std::array<double,2>& p){
                        // isSideways 將垂直 origin 置於基線，再向左旋轉；呼叫端可整段向右旋轉 90 度。
                        return sideways?std::array<double,2>{p[1]+x+metrics[i][6]*scale,-p[0]+y+metrics[i][1]*scale*.5}:std::array<double,2>{p[0]+x,p[1]+y};
                    };
                    for(const auto& contour:glyph.contours){
                        if(commands.size()>1048576)throw std::bad_alloc{};auto current=position(contour.start);commands.push_back({5,point_bits(current),0,0});
                        for(const auto& segment:contour.segments){
                            const unsigned slot=segment.kind==1?6:7;const auto offset_at=data.size();
                            if(segment.kind==1){current=position(segment.points[0]);append(point_bits(current));}
                            else if(segment.kind==2){
                                const auto control=position(segment.points[0]),end=position(segment.points[1]);
                                // SimplifiedGeometrySink 只有 cubic；二次曲線以等價的三次控制點輸出。
                                std::array<double,2> a{},b{};for(unsigned j=0;j<2;++j){a[j]=current[j]+(control[j]-current[j])*2/3;b[j]=end[j]+(control[j]-end[j])*2/3;}
                                append(point_bits(a));append(point_bits(b));append(point_bits(end));current=end;
                            }else {for(unsigned j=0;j<3;++j)append(point_bits(position(segment.points[j])));current=position(segment.points[2]);}
                            if(!commands.empty() && commands.back().slot==slot && commands.back().offset+commands.back().count*(slot==6?8:24)==offset_at)++commands.back().count;
                            else {if(commands.size()>=1048576)throw std::bad_alloc{};commands.push_back({slot,0,offset_at,1});}
                        }
                        commands.push_back({8,0,0,1});
                    }
                    if(!rtl)cursor+=advance;
                }}catch(const std::invalid_argument&){return finish(invalid);}catch(const std::length_error&){return finish(invalid);}
                const auto scratch=allocate_heap(memory,std::max<std::size_t>(1,data.size()));if(!scratch)return finish(0x8007000eU);
                try{
                    if(!data.empty())memory.write(GuestAddress{scratch},data);std::vector<GuestCallback> callbacks;callbacks.reserve(commands.size());
                    for(const auto& command:commands){
                        if(command.slot==5)callbacks.push_back(font_callback(memory,unit,arg(8),5,{command.point,0},2));
                        else if(command.slot==6 || command.slot==7)callbacks.push_back(font_callback(memory,unit,arg(8),command.slot,{scratch+command.offset,command.count}));
                        else callbacks.push_back(font_callback(memory,unit,arg(8),command.slot,{command.count}));
                    }
                    // Sink 的 Close 由呼叫端負責；void 回呼的 RAX 不得當成 HRESULT。
                    start_callbacks(state,memory,std::move(callbacks),Continuation::ReturnZeroFreeHeap,scratch);
                }catch(...){(void)free_heap(memory,scratch,state);throw;}return std::nullopt;
            }
            if(method=="GetDesignGlyphMetrics" || method=="GetGdiCompatibleGlyphMetrics"){
                const bool gdi=method=="GetGdiCompatibleGlyphMetrics";
                const auto input=arg(gdi?5:1),output=arg(gdi?7:3);const auto count=static_cast<std::uint32_t>(arg(gdi?6:2));
                if(count>1048576)return finish(invalid);if(count && (!input || !output))return finish(pointer_error);
                if(count){memory.check(GuestAddress{input},count*2U,Access::Read);memory.check(GuestAddress{output},count*28U,Access::Write);}
                double sx=1,sy=1;bool natural=false;
                if(gdi){
                    const double em=floating(1),ppd=floating(2);sx=sy=em*ppd/face.em;natural=arg(4)!=0;
                    if(em<=0 || ppd<=0 || !std::isfinite(sx) || sx<=0)return finish(invalid);
                    if(arg(3)){
                        std::array<float,6> matrix{};for(unsigned i=0;i<6;++i)matrix[i]=std::bit_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)+i*4},32)));
                        if(std::any_of(matrix.begin(),matrix.end(),[](float v){return !std::isfinite(v);}))return finish(invalid);
                        if(matrix[1]!=0 || matrix[2]!=0)return finish(0x80004001U);
                        sx*=std::abs(matrix[0]);sy*=std::abs(matrix[3]);if(sx<=0 || sy<=0)return finish(invalid);
                    }
                }
                std::vector<std::uint16_t> indices(count);for(unsigned i=0;i<count;++i)indices[i]=static_cast<std::uint16_t>(memory.load(GuestAddress{input+i*2},16));
                std::vector<NativeGlyphMetrics> metrics;
                try{metrics=native_font_glyph_metrics(face,indices);}catch(const std::invalid_argument&){return finish(invalid);}
                if(gdi){
                    // CoreText 設計量測轉為軸向像素對齊量測；沒有宣稱重現 Windows 的 TrueType hint interpreter。
                    // Natural 保留水平 fractional advance；一般 GDI 對齊整像素，兩者仍輸出設計單位。
                    const auto fit=[](double value,double scale){return std::floor(std::floor(value*scale+0.5)/scale+0.5);};
                    for(auto& m:metrics){
                        const double horizontal_scale=sx*(natural?3:1);
                        const double left=fit(m[0],horizontal_scale),right=fit(static_cast<double>(m[1])-m[2],horizontal_scale);
                        const double top=fit(static_cast<double>(m[6])-m[3],sy),bottom=fit(static_cast<double>(m[5])-m[4]+m[6],sy);
                        const double width=natural?m[1]:fit(m[1],sx),height=fit(m[4],sy),origin=fit(m[6],sy);
                        const std::array<double,7> fitted{left,width,width-right,origin-top,height,height-origin+bottom,origin};
                        for(unsigned i=0;i<7;++i){if(!std::isfinite(fitted[i]) || fitted[i]<INT32_MIN || fitted[i]>INT32_MAX)return finish(invalid);m[i]=static_cast<std::int32_t>(fitted[i]);}
                    }
                }
                for(unsigned i=0;i<count;++i)for(unsigned j=0;j<7;++j)memory.store(GuestAddress{output+i*28+j*4},32,static_cast<std::uint32_t>(metrics[i][j]));
                return finish(0);
            }
            if(method=="GetRecommendedRenderingMode"){
                if(!arg(5))return finish(pointer_error);memory.store(GuestAddress{arg(5)},32,0);
                const auto em=floating(1),ppd=floating(2);if(!std::isfinite(em) || !std::isfinite(ppd) || em<=0 || ppd<=0 || arg(3)>2)return finish(invalid);
                const auto params=bridge.objects.find(arg(4));if(params==bridge.objects.end() || params->second.kind!=Kind::RenderingParams)return finish(invalid);
                constexpr std::array<unsigned,3> modes{4,2,3};const auto mode=params->second.rendering_mode?params->second.rendering_mode:modes[static_cast<unsigned>(arg(3))];
                memory.store(GuestAddress{arg(5)},32,mode);return finish(0);
            }
            if(method=="GetGdiCompatibleMetrics"){
                if(!arg(4))return finish(pointer_error);
                memory.check(GuestAddress{arg(4)},20,Access::Write);
                const double em=floating(1),ppd=floating(2);double vertical=1;
                if(arg(3)){
                    memory.check(GuestAddress{arg(3)},24,Access::Read);
                    std::array<float,6> matrix{};for(unsigned i=0;i<6;++i)matrix[i]=std::bit_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)+i*4},32)));
                    if(std::any_of(matrix.begin(),matrix.end(),[](float v){return !std::isfinite(v);}))return finish(invalid);
                    // 目前提供軸向縮放；旋轉／斜切的 GDI fitting 尚未接到原生後端。
                    if(matrix[1]!=0 || matrix[2]!=0)return finish(0x80004001U);
                    if(matrix[3]!=0)vertical=std::abs(matrix[3]);
                }
                const double scale=em*ppd*vertical/face.em;
                if(!std::isfinite(scale) || em<=0 || ppd<=0 || scale<=0){memory.write(GuestAddress{arg(4)},std::array<std::uint8_t,20>{});return finish(invalid);}
                auto metrics=face.design_metrics;
                for(unsigned i=1;i<10;++i){
                    const auto fitted=std::floor(std::floor(metrics[i]*scale+0.5)/scale+0.5);
                    const auto low=(i==3 || i==6 || i==8)?INT16_MIN:0,high=(i==3 || i==6 || i==8)?INT16_MAX:UINT16_MAX;
                    if(!std::isfinite(fitted) || fitted<low || fitted>high)return finish(invalid);
                    metrics[i]=static_cast<std::int32_t>(fitted);
                }
                for(unsigned i=0;i<10;++i)memory.store(GuestAddress{arg(4)+i*2},16,static_cast<std::uint16_t>(metrics[i]));return finish(0);
            }
            if(method=="GetMetrics"){
                memory.check(GuestAddress{arg(1)},20,Access::Write);
                for(unsigned i=0;i<10;++i)memory.store(GuestAddress{arg(1)+i*2},16,static_cast<std::uint16_t>(face.design_metrics[i]));return finish(0);
            }
            if(method=="GetWeight")return finish(static_cast<std::uint32_t>(face.weight));
            if(method=="GetStretch")return finish(font_stretch(face));
            if(method=="GetStyle")return finish(face.italic?2:0);
            if(method=="GetSimulations")return finish(o.simulations);
            if(method=="IsSymbolFont")return finish((face.signature[4]&0x80000000U)!=0);
            if(method=="GetFaceNames"){if(!arg(1))return finish(pointer_error);out(1,strings(face.style));return finish(0);}
            if(method=="GetInformationalStrings"){
                if(!arg(2) || !arg(3))return finish(pointer_error);memory.check(GuestAddress{arg(2)},unit,Access::Write);memory.check(GuestAddress{arg(3)},4,Access::Write);
                out(2,0);memory.store(GuestAddress{arg(3)},32,0);const auto id=arg(1);
                constexpr std::array<std::uint16_t,22> names{UINT16_MAX,0,5,7,8,9,12,10,11,13,14,1,2,16,17,19,4,6,20,21,23,24};
                if(id>=names.size())return finish(invalid);if(!id)return finish(0);Object value;value.kind=Kind::LocalizedStrings;
                try{value.localized=native_font_names(face,names[id]);}catch(const std::invalid_argument&){return finish(0x88985000U);}
                if(!value.localized.empty()){out(2,create(std::move(value)));memory.store(GuestAddress{arg(3)},32,1);}return finish(0);
            }
            if(method=="CreateFontFace" || method=="CreateFontFace3"){if(!arg(1))return finish(pointer_error);Object value;value.kind=Kind::FontFace;value.blob=o.blob;value.file_index=o.file_index;value.fonts=o.fonts;value.indices=o.indices;value.simulations=o.simulations;out(1,create(std::move(value)));return finish(0);}
            if(method=="GetFontFamily"){if(!arg(1))return finish(pointer_error);Object value;value.kind=Kind::FontFamily;value.fonts=o.fonts;value.text=face.family;for(std::size_t i=0;i<o.fonts->faces.size();++i)if(o.fonts->faces[i].family==face.family)value.indices.push_back(i);out(1,create(std::move(value)));return finish(0);}
        }
        if(o.kind==Kind::LocalizedStrings || o.kind==Kind::StringList){
            if(method=="GetCount")return finish(o.localized.size());
            if(method=="FindLocaleName"){
                if(!arg(1)||!arg(2)||!arg(3))return finish(pointer_error);const auto requested=folded(read_utf16(memory,GuestAddress{arg(1)},85));
                const auto found_name=std::find_if(o.localized.begin(),o.localized.end(),[&](const auto& item){return folded(item.locale)==requested;});const bool exists=found_name!=o.localized.end();
                memory.store(GuestAddress{arg(2)},32,exists?static_cast<std::uint32_t>(found_name-o.localized.begin()):UINT32_MAX);memory.store(GuestAddress{arg(3)},32,exists);return finish(0);
            }
            if(arg(1)>=o.localized.size())return finish(invalid);
            const auto& text=method.starts_with("GetLocaleName")?o.localized[arg(1)].locale:o.localized[arg(1)].value;
            if(!arg(2))return finish(pointer_error);
            if(method.ends_with("Length")){memory.store(GuestAddress{arg(2)},32,text.size());return finish(0);}
            if(method=="GetString" || method=="GetLocaleName"){if(arg(3)<=text.size())return finish(insufficient);memory.write(GuestAddress{arg(2)},utf16_bytes(text));return finish(0);}
        }
        throw HostCallFault("尚未實作 DirectWrite 方法："+std::string(name));
    }catch(const std::invalid_argument&){cleanup_fallback();return finish(invalid);}
    catch(const std::bad_alloc&){cleanup_fallback();return finish(0x8007000eU);}
}
}
