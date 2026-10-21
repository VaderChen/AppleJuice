#include "x64arm/windows_services.hpp"
#include "x64arm/windows_metafile.hpp"
#include "x64arm/windows_geometry.hpp"
#include <algorithm>
namespace x64arm {
struct MetafileState {
    struct Enumeration {EmfDecoder decoder;std::uint64_t dc{};unsigned count{};GuiTransform mapping{gui_identity};GuiDc original;std::vector<GuiDc> saved;std::map<unsigned,std::uint64_t> objects;};
    std::map<std::uint64_t,Enumeration> enumerations;
};
void WindowsServices::finish_metafile_enumeration(std::uint64_t table){
    if(!metafile_playback_)return;const auto found=metafile_playback_->enumerations.find(table);if(found==metafile_playback_->enumerations.end())return;const auto& operation=found->second;
    if(operation.dc && gui_.contexts.contains(operation.dc)){gui_.contexts[operation.dc]=operation.original;gui_.dc_objects[operation.dc]=operation.original.font;gui_.saved_contexts[operation.dc]=operation.saved;}
    for(const auto& [index,handle]:operation.objects){(void)index;gui_.pens.erase(handle);gui_.pen_widths.erase(handle);gui_.brushes.erase(handle);gui_.fonts.erase(handle);}metafile_playback_->enumerations.erase(found);
}

std::span<const WindowsServiceDescriptor> metafile_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"gdi32.dll","GdiComment",3},{"gdi32.dll","PlayEnhMetaFileRecord",4},{"gdi32.dll","CreateEnhMetaFileA",4},{"gdi32.dll","CreateEnhMetaFileW",4},{"gdi32.dll","CloseEnhMetaFile",1},{"gdi32.dll","DeleteEnhMetaFile",1},
        {"gdi32.dll","CopyEnhMetaFileA",2},{"gdi32.dll","CopyEnhMetaFileW",2},{"gdi32.dll","GetEnhMetaFileHeader",3},{"gdi32.dll","GetEnhMetaFileDescriptionA",3},{"gdi32.dll","GetEnhMetaFileDescriptionW",3},
        {"gdi32.dll","GetEnhMetaFileBits",3},{"gdi32.dll","SetEnhMetaFileBits",2},{"gdi32.dll","GetEnhMetaFilePaletteEntries",3},{"gdi32.dll","EnumEnhMetaFile",5},{"gdi32.dll","PlayEnhMetaFile",3}
    };return table;
}
bool WindowsServices::record_metafile_draw(std::uint64_t hdc,const GuiDraw& draw){
    const auto found=gui_.emf_recorders.find(hdc);if(found==gui_.emf_recorders.end())return false;
    try{emf_draw(found->second,draw);}catch(const std::invalid_argument& e){throw HostCallFault(e.what());}return true;
}
std::optional<std::uint32_t> WindowsServices::invoke_metafile(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const unsigned unit=x86_?4:8;
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto fail=[&](std::uint32_t error){last_error_=error;return finish(name.starts_with("GetEnhMetaFileDescription") || name=="GetEnhMetaFilePaletteEntries"?UINT32_MAX:0);};
    const auto valid_dc=[&](std::uint64_t h){return gui_.contexts.contains(h) || gui_.dc_objects.contains(h) || (h>=0x10000000 && gui_.windows.contains(h-0x10000000));};
    const auto rect=[&](std::uint64_t address){GuiRect out{};for(unsigned i=0;i<4;++i)out[i]=static_cast<std::int32_t>(memory.load(GuestAddress{address+i*4},32));return out;};
    const auto paint=[&](std::uint64_t target_dc,GuiDraw draw,const GuiTransform& mapping){
        auto& dc=gui_.contexts[target_dc];const auto surface=gui_.emf_recorders.contains(target_dc)?std::shared_ptr<RasterSurface>{}:raster_surface(memory,target_dc);const auto window=gui_.windows.find(target_dc-0x10000000);
        if(!surface && !gui_.emf_recorders.contains(target_dc) && window==gui_.windows.end())throw std::domain_error("EMF 沒有繪圖表面");draw.transform=gui_multiply(draw.transform,mapping);std::optional<GuiRegion> clip;
                if(draw.clipped){clip=gui_transform_region_rect(mapping,draw.clip);for(const auto& r:draw.excluded)*clip=gui_region_combine(*clip,gui_transform_region_rect(mapping,r),4);}if(dc.clipped){const auto region=gui_region_combine(GuiRegion{dc.clip},dc.excluded,4);clip=clip?gui_region_combine(*clip,region,1):region;}
                draw.clipped=clip.has_value();draw.excluded.clear();if(clip){draw.clip=gui_region_bounds(*clip);draw.excluded=gui_region_combine(GuiRegion{draw.clip},*clip,4);}
                if(record_metafile_draw(target_dc,draw))return;if(surface)surface->draw(draw);else window->second.drawing.push_back(std::move(draw));};
    try{
        if(name.starts_with("CreateEnhMetaFile")){
            if(arg(0) && !valid_dc(arg(0)))return fail(6);if(arg(1))return fail(50);
            GuiMetafile meta;if(arg(2)){meta.frame=rect(arg(2));if(gui_rect_empty(*meta.frame))return fail(87);}
            if(arg(3)){const bool wide=name.ends_with('W');bool zero=false,done=false;std::string narrow;
                for(unsigned i=0;i<32768;++i){const auto c=memory.load(GuestAddress{arg(3)+i*(wide?2:1)},wide?16:8);if(wide)meta.description.push_back(static_cast<char16_t>(c));else narrow.push_back(static_cast<char>(c));if(!c && zero){done=true;break;}zero=!c;}
                if(!done)return fail(87);if(!wide)meta.description=utf8_to_utf16_lossy(narrow);
                meta.description.pop_back(); // 終止符由共用 UTF-16 encoder 補上。
            }
            emf_begin(meta);const auto handle=gui_.next_object++;gui_.emf_recorders.emplace(handle,std::move(meta));try{gui_.contexts.emplace(handle,GuiDc{});}catch(...){gui_.emf_recorders.erase(handle);throw;}return finish(handle);
        }
        if(name=="CloseEnhMetaFile"){
            const auto found=gui_.emf_recorders.find(arg(0));if(found==gui_.emf_recorders.end())return fail(6);auto meta=found->second;emf_finish(meta);const auto handle=gui_.next_object++;gui_.metafiles.emplace(handle,std::move(meta));gui_.emf_recorders.erase(found);gui_.contexts.erase(arg(0));gui_.dc_objects.erase(arg(0));gui_.saved_contexts.erase(arg(0));return finish(handle);
        }
        if(name=="SetEnhMetaFileBits"){
            if(!arg(1) || arg(0)>16*1024*1024 || arg(0)<88)return fail(87);GuiMetafile meta;meta.bytes.resize(static_cast<std::size_t>(arg(0)));memory.read(GuestAddress{arg(1)},meta.bytes);const auto records=emf_validate(meta.bytes);meta.records=static_cast<unsigned>(records.size());meta.imported=true;
            GuiRect frame{},bounds{};for(unsigned i=0;i<4;++i){frame[i]=static_cast<std::int32_t>(emf_get(meta.bytes,24+i*4));bounds[i]=static_cast<std::int32_t>(emf_get(meta.bytes,8+i*4));}meta.frame=frame;meta.bounds=bounds;
            const auto count=emf_get(meta.bytes,60),offset=emf_get(meta.bytes,64);for(unsigned i=0;i<count;++i){const auto at=offset+i*2;meta.description.push_back(static_cast<char16_t>(meta.bytes[at]|(static_cast<unsigned>(meta.bytes[at+1])<<8)));}if(!meta.description.empty())meta.description.pop_back();const auto handle=gui_.next_object++;gui_.metafiles.emplace(handle,std::move(meta));return finish(handle);
        }
        if(name=="GdiComment"){
            const auto found=gui_.emf_recorders.find(arg(0));if(found==gui_.emf_recorders.end())return fail(6);const auto count=static_cast<std::uint32_t>(arg(1));if(count>16*1024*1024-12 || (count && !arg(2)))return fail(87);
            auto record=emf_record(70,(12+count+3)&~3U);emf_put(record,8,count);if(count)memory.read(GuestAddress{arg(2)},std::span(record).subspan(12,count));emf_append(found->second,record);return finish(1);
        }
        if(name=="PlayEnhMetaFileRecord"){
            if(!valid_dc(arg(0)) || !arg(1) || !arg(2) || !metafile_playback_)return fail(6);const auto found=metafile_playback_->enumerations.find(arg(1));if(found==metafile_playback_->enumerations.end())return fail(87);auto& operation=found->second;
            if(arg(0)!=operation.dc || static_cast<unsigned>(arg(3))!=operation.count)return fail(87);
            const auto size=static_cast<unsigned>(memory.load(GuestAddress{arg(2)+4},32));if(size<8 || size%4 || size>16*1024*1024)return fail(13);std::vector<std::uint8_t> record(size);memory.read(GuestAddress{arg(2)},record);const auto type=emf_get(record,0);
            // 真正未知的未來記錄依 API 契約略過；已知但未實作的圖形記錄仍明確失敗。
            if(type>122)return finish(1);unsigned index=0;if(type==38 || type==39 || type==82 || type==37 || type==40){index=emf_get(record,8);if(!(index&0x80000000U) && (!index || index>=operation.count))return fail(13);}
            auto changed=operation.decoder;const auto draws=emf_decode_record(changed,record);
            if(type==38 || type==39 || type==82){
                memory.check(GuestAddress{arg(1)+index*unit},unit,Access::Write);const auto handle=gui_.next_object++;const auto& object=changed.objects.at(index);
                if(type==38){gui_.pens[handle]=object.color;gui_.pen_widths[handle]=object.empty?0:std::max(1,object.width);}
                else if(type==39){if(object.empty){operation.objects[index]=0xb005;memory.store(GuestAddress{arg(1)+index*unit},static_cast<std::uint8_t>(unit*8),0xb005);}else gui_.brushes[handle]=object.color;}
                else gui_.fonts[handle]=std::vector<std::uint8_t>(record.begin()+12,record.begin()+104);
                if(type!=39 || !object.empty){operation.objects[index]=handle;memory.store(GuestAddress{arg(1)+index*unit},static_cast<std::uint8_t>(unit*8),handle);}
            }
            if(type==37 && !(index&0x80000000U)){
                const auto object=operation.objects.find(index);if(object==operation.objects.end() || memory.load(GuestAddress{arg(1)+index*unit},static_cast<std::uint8_t>(unit*8))!=object->second)return fail(50);
            }
            if(type==40){const auto object=operation.objects.find(index);if(object==operation.objects.end())return fail(13);const auto handle=object->second;gui_.pens.erase(handle);gui_.pen_widths.erase(handle);gui_.brushes.erase(handle);gui_.fonts.erase(handle);operation.objects.erase(object);memory.store(GuestAddress{arg(1)+index*unit},static_cast<std::uint8_t>(unit*8),0);}
            operation.decoder=std::move(changed);const auto& current=operation.decoder.current;auto& dc=gui_.contexts[arg(0)];
            const auto selected=[&](unsigned object){return object&0x80000000U?UINT64_C(0xb000)+(object&0x7fffffffU):operation.objects.contains(object)?operation.objects.at(object):UINT64_C(0);};
            dc.pen=selected(current.pen_object);dc.brush=selected(current.brush_object);dc.font=selected(current.font_object);gui_.dc_objects[arg(0)]=dc.font;dc.rop2=current.rop2;dc.color=current.text;dc.mode=current.mode;dc.alignment=current.alignment;dc.x=current.point[0];dc.y=current.point[1];dc.graphics_mode=2;dc.map_mode=1;dc.window_origin=dc.viewport_origin={0,0};dc.world=gui_multiply(current.transform,operation.mapping);
            if(type==70 && gui_.emf_recorders.contains(arg(0)))emf_append(gui_.emf_recorders.at(arg(0)),record);
            for(auto draw:draws)paint(arg(0),std::move(draw),operation.mapping);if(gui_.windows.contains(arg(0)-0x10000000))present_gui(arg(0)-0x10000000);return finish(1);
        }
        const auto handle=arg(name=="PlayEnhMetaFile" || name=="EnumEnhMetaFile"?1:0);const auto found=gui_.metafiles.find(handle);if(found==gui_.metafiles.end())return fail(6);const auto& meta=found->second;
        if(name=="DeleteEnhMetaFile"){gui_.metafiles.erase(found);return finish(1);}
        if(name.starts_with("CopyEnhMetaFile")){if(arg(1))return fail(50);const auto copy=gui_.next_object++;gui_.metafiles.emplace(copy,meta);return finish(copy);}
        if(name=="GetEnhMetaFilePaletteEntries"){if(emf_get(meta.bytes,68))return fail(50);return finish(0);}
        if(name=="GetEnhMetaFileBits" || name=="GetEnhMetaFileHeader"){
            const auto size=name=="GetEnhMetaFileBits"?meta.bytes.size():std::size_t{emf_get(meta.bytes,4)};if(!arg(2))return finish(size);if(arg(1)<size && name=="GetEnhMetaFileBits")return fail(122);const auto count=std::min<std::size_t>(size,static_cast<std::size_t>(arg(1)));memory.write(GuestAddress{arg(2)},std::span(meta.bytes).first(count));return finish(count);
        }
        if(name.starts_with("GetEnhMetaFileDescription")){
            if(meta.description.empty())return finish(0);const bool wide=name.ends_with('W');std::vector<std::uint8_t> bytes;if(wide)bytes=utf16_bytes(meta.description);else {const auto text=utf16_to_utf8(meta.description);bytes.assign(text.begin(),text.end());bytes.push_back(0);}const auto characters=bytes.size()/(wide?2:1);if(!arg(2))return finish(characters);const auto count=std::min<std::size_t>(static_cast<std::size_t>(arg(1)),characters);memory.write(GuestAddress{arg(2)},std::span(bytes).first(count*(wide?2:1)));return finish(count);
        }
        if(name=="EnumEnhMetaFile"){
            if((arg(0) && !valid_dc(arg(0))) || !arg(2))return fail(87);if(arg(0) && !arg(4))return fail(87);if(arg(0) && arg(4))memory.check(GuestAddress{arg(4)},16,Access::Read);
            const auto records=emf_validate(meta.bytes);const auto handles=std::max(1U,emf_get(meta.bytes,56)&65535U);const auto allocation=allocate_heap(memory,static_cast<std::size_t>(handles)*unit+meta.bytes.size());if(!allocation)return fail(8);
            try{
                if(!metafile_playback_)metafile_playback_=std::make_shared<MetafileState>();if(metafile_playback_->enumerations.size()>=256)throw std::length_error("EMF 巢狀列舉過深");MetafileState::Enumeration operation;operation.dc=arg(0);operation.count=handles;
                if(arg(0)){operation.original=gui_.contexts[arg(0)];operation.saved=gui_.saved_contexts[arg(0)];const auto target=rect(arg(4));if(gui_rect_empty(target))throw std::invalid_argument("EMF 目標矩形為空");const auto source=meta.frame?gui_transform_bounds({96.0/2540,0,0,96.0/2540,0,0},*meta.frame):meta.bounds.value_or(GuiRect{});operation.mapping=gui_device_transform(operation.original);if(!gui_rect_empty(source)){const double sx=(static_cast<double>(target[2])-target[0])/(static_cast<double>(source[2])-source[0]),sy=(static_cast<double>(target[3])-target[1])/(static_cast<double>(source[3])-source[1]);operation.mapping=gui_multiply({sx,0,0,sy,target[0]-source[0]*sx,target[1]-source[1]*sy},operation.mapping);}}
                metafile_playback_->enumerations.emplace(allocation,std::move(operation));memory.write(GuestAddress{allocation},std::vector<std::uint8_t>(static_cast<std::size_t>(handles)*unit));memory.store(GuestAddress{allocation},static_cast<std::uint8_t>(unit*8),handle);const auto data=allocation+static_cast<std::size_t>(handles)*unit;memory.write(GuestAddress{data},meta.bytes);std::vector<GuestCallback> callbacks;callbacks.reserve(records.size());for(const auto& [offset,size]:records){(void)size;callbacks.push_back({arg(2),{arg(0),allocation,data+offset,handles,arg(3)}});}start_callbacks(state,memory,std::move(callbacks),Continuation::MetafileEnumeration,allocation);return std::nullopt;}catch(...){finish_metafile_enumeration(allocation);free_heap(memory,allocation,state);throw;}
        }
        if(name=="PlayEnhMetaFile"){
            if(!valid_dc(arg(0)) || !arg(2))return fail(87);
            const auto target=rect(arg(2));if(gui_rect_empty(target))return fail(87);const auto source=meta.frame?gui_transform_bounds({96.0/2540,0,0,96.0/2540,0,0},*meta.frame):meta.bounds.value_or(GuiRect{});if(gui_rect_empty(source))return finish(1);
            auto& dc=gui_.contexts[arg(0)];const double sx=(static_cast<double>(target[2])-target[0])/(static_cast<double>(source[2])-source[0]),sy=(static_cast<double>(target[3])-target[1])/(static_cast<double>(source[3])-source[1]);const auto mapping=gui_multiply({sx,0,0,sy,target[0]-source[0]*sx,target[1]-source[1]*sy},gui_device_transform(dc));
            const auto draws=emf_decode(meta.bytes);const auto surface=gui_.emf_recorders.contains(arg(0))?std::shared_ptr<RasterSurface>{}:raster_surface(memory,arg(0));const auto window=gui_.windows.find(arg(0)-0x10000000);if(!surface && !gui_.emf_recorders.contains(arg(0)) && window==gui_.windows.end())return fail(50);
            if(gui_.emf_recorders.contains(arg(0))){EmfDecoder decoder;for(const auto& [offset,size]:emf_validate(meta.bytes)){const auto record=std::span(meta.bytes).subspan(offset,size);auto decoded=emf_decode_record(decoder,record);if(emf_get(record,0)==70)emf_append(gui_.emf_recorders.at(arg(0)),std::vector<std::uint8_t>(record.begin(),record.end()));for(auto draw:decoded)paint(arg(0),std::move(draw),mapping);}}
            else for(auto draw:draws)paint(arg(0),std::move(draw),mapping);
            if(window!=gui_.windows.end())present_gui(window->first);return finish(1);
        }
        return fail(50);
    }catch(const std::domain_error&){return fail(50);}catch(const std::bad_alloc&){return fail(8);}catch(const std::length_error&){return fail(8);}catch(const std::invalid_argument&){return fail(13);}
}
}
