#include "ansi_codepage.hpp"
#include "x64arm/windows_services.hpp"
#include "x64arm/windows_guid.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_window_state.hpp"
#include "x64arm/windows_gui_abi.hpp"
#include "avi_container.hpp"
#include "avi_edit_list.hpp"
#include "avi_clipboard_media.hpp"
#include "vfw_codec.hpp"
#include "vfw_compression.hpp"
#include "vfw_drawdib.hpp"
#include "dialog_template.hpp"
#include "mciwnd.hpp"
#include "service_callback_task.hpp"
#include <algorithm>

namespace x64arm {
namespace {
using avi::Bytes;
using avi::word;
using avi::put;
constexpr unsigned unsupported=0x80044065,bad_format=0x80044066,no_memory=0x80044067,
    bad_parameter=0x8004406a,bad_size=0x8004406b,bad_handle=0x8004406c,read_error=0x8004406d,
    write_error=0x8004406e,open_error=0x8004406f,readonly=0x80044072,no_data=0x80044073,
    too_small=0x80044074,no_interface=0x80004002,pointer_error=0x80004003;
constexpr std::size_t memory_limit=256U*1024*1024;
enum class Kind {file,stream,persist,factory,composition,editable,edit};
constexpr std::array<std::string_view,7> families{"internal-avi-file","internal-avi-stream","internal-avi-persist","internal-avi-factory","internal-avi-composition","internal-avi-editable","internal-avi-edit"};

struct DiskMovie {
    avi::Movie movie;
    std::shared_ptr<HostFile> handle;
    std::u16string path;
    bool writable{},dirty{},memory_backed{};
    std::size_t payload{};
    unsigned record{};
    unsigned write_to(HostFile& file){
        const auto bytes=avi::encode(movie);
        const auto result=file.write(bytes,0);
        if(result.error || result.bytes!=bytes.size() || file.truncate(bytes.size()) || file.flush())return write_error;
        return 0;
    }
    unsigned flush(){
        if(!dirty)return 0;
        const auto hr=handle?write_to(*handle):bad_handle;
        if(!hr)dirty=false;
        return hr;
    }
};
struct DiskObject {
    Kind kind;
    std::shared_ptr<DiskMovie> disk;
    unsigned stream{},refs{1};
};
constexpr std::array<unsigned,12> guest_offsets{0,4,8,16,36,20,24,28,32,40,44,48};
avi::Stream unpack_stream(avi::View info){
    avi::Stream stream;
    for(unsigned i=0;i<guest_offsets.size();++i)put(stream.header,i*4,word(info,guest_offsets[i]));
    for(unsigned i=0;i<4;++i){
        auto value=word(info,52+4*i);
        stream.header[48+2*i]=static_cast<std::uint8_t>(value);stream.header[49+2*i]=static_cast<std::uint8_t>(value>>8);
    }
    std::u16string name;
    for(unsigned i=76;i+1<info.size();i+=2){
        const auto c=char16_t(info[i]|(info[i+1]<<8));if(!c)break;name+=c;
    }
    const auto encoded=utf16_to_utf8_lossy(name);
    stream.name.assign(encoded.begin(),encoded.end());if(!encoded.empty())stream.name.push_back(0);
    return stream;
}
Bytes pack_stream(const avi::Stream& stream){
    Bytes info(204);
    for(unsigned i=0;i<guest_offsets.size();++i)put(info,guest_offsets[i],word(stream.header,i*4));
    for(unsigned i=0;i<4;++i){
        const auto p=48+i*2;
        put(info,52+4*i,static_cast<unsigned>(static_cast<std::int16_t>(stream.header[p]|stream.header[p+1]<<8)));
    }
    const std::string name(stream.name.begin(),std::find(stream.name.begin(),stream.name.end(),0));
    const auto text=utf16_bytes(utf8_to_utf16_lossy(name).substr(0,63));
    std::copy(text.begin(),text.end(),info.begin()+76);
    return info;
}
void update_main(avi::Movie& movie){
    std::uint32_t largest=0;
    for(const auto& s:movie.streams)largest=std::max(largest,word(s.header,36));
    put(movie.header,28,largest);
    for(const auto& s:movie.streams)if(word(s.header,0)==avi::video){
        const auto rate=word(s.header,24);
        put(movie.header,0,rate?static_cast<unsigned>(1000000ULL*word(s.header,20)/rate):0);
        put(movie.header,16,word(s.header,32));
        if(s.format.size()>=40){
            put(movie.header,32,word(s.format,4));
            auto h=word(s.format,8);put(movie.header,36,h>INT32_MAX?0U-h:h);
        }
        break;
    }
}
}
struct WindowsServices::AviDiskState {
    struct Composition {
        std::vector<std::uint64_t> streams;
        std::array<std::uint8_t,172> info{};
        unsigned refs{1};
    };
    std::map<std::uint64_t,std::shared_ptr<Composition>> compositions;
    std::map<std::uint64_t,DiskObject> objects;
    std::array<std::uint64_t,families.size()> tables{};
    struct Frame {
        std::uint64_t stream{},buffer{},destination{};
        std::uint32_t references{1},position{UINT32_MAX};
        avi::Bytes requested;
        int x{},y{},width{-1},height{-1};
        bool fixed{};
    };
    std::map<std::uint64_t,std::shared_ptr<Frame>> frames;
    std::uint64_t frame_table{};
    std::map<std::uint64_t,std::shared_ptr<avi::EditList>> edits;
    std::map<std::uint64_t,std::shared_ptr<vfw::Codec>> codecs;
    std::map<std::uint64_t,std::shared_ptr<vfw::CompressionSequence>> compression_sequences;
    std::map<std::uint64_t,std::uint64_t> compressor_outputs;
    std::map<std::uint64_t,std::shared_ptr<vfw::CompressorChoice>> compressor_dialogs;
    std::map<std::pair<std::uint32_t,std::uint32_t>,vfw::Driver> codec_overrides;
    std::map<std::uint64_t,std::shared_ptr<vfw::DrawDib>> drawdibs;
    std::map<std::uint64_t,std::shared_ptr<vfw::MciWindow>> mci_windows;
    unsigned locks{};
};
bool WindowsServices::begin_avi_disk(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);
    if(!call(0))return false;
    WindowsGuid id{};memory.read(GuestAddress{call(0)},id);
    if(id!=ole_standard_guid(0x20000))return false;
    invoke_avi_disk(state,memory,service);
    return true;
}
std::optional<std::uint32_t> WindowsServices::invoke_avi_disk(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);
    const auto name=service.symbol;
    const unsigned unit=x86_?4:8;
    const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto done=[&](std::uint64_t value){call.return_to(state,value);return std::optional<std::uint32_t>{};};
    if(!avi_disk_)avi_disk_=std::make_shared<AviDiskState>();
    auto& store=*avi_disk_;
#include "windows_vfw_mciwnd.inc"
#include "windows_vfw_choose_proc.inc"
#include "windows_vfw_codec.inc"
#include "windows_avi_frame.inc"
    auto guid=[&](std::uint64_t p){WindowsGuid id{};if(p)memory.read(GuestAddress{p},id);return id;};
    auto setptr=[&](std::uint64_t p,std::uint64_t value){memory.store(GuestAddress{p},bits,value);};
    auto number=[&](unsigned i){return static_cast<std::int32_t>(call(i));};
    auto result=[&](unsigned i,std::uint32_t value){if(call(i))memory.store(GuestAddress{call(i)},32,value);};
    auto table=[&](Kind kind){
        auto& address=store.tables[static_cast<unsigned>(kind)];
        if(!address){
            auto family=families[static_cast<unsigned>(kind)];
            std::vector<std::uint64_t> slots;
            for(const auto& d:avifil32_service_descriptors())if(d.family==family)slots.push_back(resolve(family,d.symbol).value);
            address=runtime_allocate(memory,{},unit*slots.size());
            for(unsigned i=0;i<slots.size();++i)setptr(address+i*unit,slots[i]);
        }
        return address;
    };
#include "windows_avi_composition.inc"
#include "windows_avi_edit.inc"
#include "windows_avi_clipboard.inc"
    auto interface_offset=[&](Kind kind,WindowsGuid iid)->int{
        if(iid==ole_standard_guid(0))return 0;
        if(kind==Kind::file && (iid==ole_standard_guid(0x10b) || iid==ole_standard_guid(0x10c)))return static_cast<int>(unit);
        const auto id=kind==Kind::factory?1U:kind==Kind::stream?0x20021U:0x20020U;
        return iid==ole_standard_guid(id)?0:-1;
    };
    auto object=[&](Kind kind,WindowsGuid iid,std::uint64_t output,std::shared_ptr<DiskMovie> disk={},unsigned index=0)->unsigned{
        const auto offset=interface_offset(kind,iid);
        if(offset<0)return no_interface;
        if(kind==Kind::file)disk=std::make_shared<DiskMovie>();
        auto pointer=allocate_heap(memory,kind==Kind::file?2*unit:unit);
        if(!pointer)return no_memory;
        setptr(pointer,table(kind));
        if(kind==Kind::file)setptr(pointer+unit,table(Kind::persist));
        store.objects.emplace(pointer,DiskObject{kind,std::move(disk),index});
        setptr(output,pointer+static_cast<unsigned>(offset));
        return 0;
    };
    if(name=="DllCanUnloadNow")return done(!store.objects.empty() || !store.compositions.empty() || !store.edits.empty() || !store.frames.empty() || store.locks?1:0);
    if(name=="CoCreateInstance" || name=="CoGetClassObject" || name=="DllGetClassObject"){
        const bool direct=name=="DllGetClassObject",create=name=="CoCreateInstance";
        const auto output=call(direct?2:4);
        if(!output)return done(pointer_error);
        setptr(output,0);
        if(direct && guid(call(0))!=ole_standard_guid(0x20000))return done(0x80040111);
        if(!direct && !com_apartment(threads_.current))return done(0x800401f0);
        if(!direct && !(number(create?2:1)&1))return done(0x80040154);
        if(create && call(1))return done(0x80040110);
        return done(object(create?Kind::file:Kind::factory,guid(call(direct?1:3)),output));
    }
    const auto base=call(0)-(service.family==families[2]?unit:0);
    const auto found=store.objects.find(base);
    if(found==store.objects.end())return done(bad_handle);
    auto& self=found->second;
    if(name=="AddRef")return done(++self.refs);
    if(name=="Release"){
        if(--self.refs)return done(self.refs);
        if(self.disk && self.disk.use_count()==1)self.disk->flush();
        store.objects.erase(found);free_heap(memory,base,state);return done(0);
    }
    if(name=="QueryInterface"){
        if(!call(2))return done(pointer_error);
        setptr(call(2),0);
        const auto offset=interface_offset(self.kind,guid(call(1)));
        if(offset<0)return done(no_interface);
        ++self.refs;setptr(call(2),base+static_cast<unsigned>(offset));return done(0);
    }
    if(self.kind==Kind::factory){
        if(name=="LockServer"){
            if(number(1))++store.locks;else if(store.locks)--store.locks;
            return done(0);
        }
        if(!call(3))return done(pointer_error);
        setptr(call(3),0);
        if(call(1))return done(0x80040110);
        return done(object(Kind::file,guid(call(2)),call(3)));
    }
    auto& disk=*self.disk;
    auto& movie=disk.movie;
    if(service.family==families[2]){
        if(name=="GetClassID"){
            if(!call(1))return done(pointer_error);
            memory.write(GuestAddress{call(1)},ole_standard_guid(0x20000));return done(0);
        }
        if(name=="IsDirty")return done(disk.dirty?0:1);
        if(name=="GetCurFile"){
            if(!call(1))return done(pointer_error);
            auto bytes=utf16_bytes(disk.path.empty()?u"*.avi":disk.path);
            auto p=nested_service(state,memory,"ole32.dll","CoTaskMemAlloc",{bytes.size()});
            setptr(call(1),p);if(!p)return done(no_memory);
            memory.write(GuestAddress{p},bytes);return done(disk.path.empty()?1:0);
        }
        if(name=="SaveCompleted")return done(0);
        if(name=="Save"){
            const auto path=call(1)?read_utf16(memory,GuestAddress{call(1)}):disk.path;
            if(path==disk.path)return done(disk.flush());
            if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許另存 AVI 檔案");
            auto dest=filesystem_.open_host(utf16_to_utf8(path),0xc0000000U,4,0x80,1);
            if(dest.error || !dest.file)return done(open_error);
            const auto hr=disk.write_to(*dest.file);
            if(!hr && call(2)){
                disk.handle=std::move(dest.file);disk.path=path;disk.writable=true;disk.dirty=false;
            }
            return done(hr);
        }
        if(disk.handle)return done(0x800704dfU);
        if(!call(1))return done(pointer_error);
        const auto path=read_utf16(memory,GuestAddress{call(1)});
        const auto mode=number(2);
        const bool create=mode&0x1000,write=(mode&3)!=0 || create;
        if(write && !runtime_.network.external_io)throw HostCallFault("比較模式不允許修改 AVI 檔案");
        const auto sharing=(mode>>4)&7;
        const auto share=sharing==1?0U:sharing==2?1U:sharing==3?2U:3U;
        const auto filename=utf16_to_utf8(path);
        auto opened=!write && !filesystem_.writable()?filesystem_.open_reader(filename,{},{},share):
            filesystem_.open_host(filename,0x80000000U|(write?0x40000000U:0),create?2:3,0x80,share);
        if(opened.error || !opened.file || (opened.metadata.attributes&16))return done(open_error);
        if(!create){
            if(opened.metadata.size>memory_limit)return done(no_memory);
            Bytes bytes(opened.metadata.size);
            const auto r=opened.file->read(bytes,0);
            if(r.error || r.bytes!=bytes.size())return done(read_error);
            try{
                auto decoded=avi::decode(bytes);
                if(write && avi::encode(decoded)!=bytes)return done(unsupported);
                movie=std::move(decoded);
            }catch(const std::invalid_argument&){return done(bad_format);}
            disk.payload=bytes.size();
            if(!movie.packets.empty())disk.record=movie.packets.back().record;
        }
        disk.handle=std::move(opened.file);disk.path=path;disk.writable=write;disk.dirty=create;
        return done(disk.flush());
    }
    if(!disk.handle && !disk.memory_backed)return done(bad_handle);
    auto read_blob=[&](unsigned index,std::size_t size){
        Bytes bytes(size);if(size)memory.read(GuestAddress{call(index)},bytes);return bytes;
    };
    auto send_blob=[&](avi::View bytes,unsigned buffer,unsigned size)->unsigned{
        if(!call(size))return bad_parameter;
        auto capacity=static_cast<std::int32_t>(memory.load(GuestAddress{call(size)},32));
        result(size,static_cast<unsigned>(bytes.size()));
        if(!call(buffer))return 0;
        if(capacity<0 || static_cast<unsigned>(capacity)<bytes.size())return too_small;
        if(!bytes.empty())memory.write(GuestAddress{call(buffer)},bytes);
        return 0;
    };
    if(self.kind==Kind::stream && self.stream>=movie.streams.size())return done(bad_handle);
    if(name=="ReadData" || name=="WriteData"){
        auto& chunks=self.kind==Kind::file?movie.extra:movie.streams[self.stream].extra;
        const auto tag=static_cast<std::uint32_t>(call(1));
        auto it=std::find_if(chunks.begin(),chunks.end(),[&](const auto& e){return e.id==tag;});
        if(name=="ReadData")return done(it==chunks.end()?no_data:send_blob(it->bytes,2,3));
        if(!disk.writable)return done(readonly);
        if(number(3)<0 || (!call(2) && number(3)))return done(bad_parameter);
        if(disk.payload+static_cast<unsigned>(number(3))>memory_limit)return done(no_memory);
        auto bytes=read_blob(2,static_cast<unsigned>(number(3)));
        if(it==chunks.end())chunks.push_back({tag,std::move(bytes)});
        else{disk.payload-=it->bytes.size();it->bytes=std::move(bytes);}
        disk.payload+=static_cast<unsigned>(number(3));disk.dirty=true;return done(0);
    }
    if(self.kind==Kind::file){
        if(name=="EndRecord"){
            if(!disk.writable)return done(readonly);
            put(movie.header,12,word(movie.header,12)|0x100U);
            if(!disk.record || (!movie.packets.empty() && movie.packets.back().record==disk.record))++disk.record;
            disk.dirty=true;
            return done(disk.flush());
        }
        if(name=="Info"){
            if(!call(1))return done(bad_parameter);
            if(number(2)<0)return done(bad_size);
            Bytes info(172);put(info,0,word(movie.header,4));put(info,4,word(movie.header,12));
            put(info,8,disk.writable?3:1);put(info,12,static_cast<unsigned>(movie.streams.size()));put(info,16,word(movie.header,28));
            put(info,20,word(movie.header,32));put(info,24,word(movie.header,36));
            if(!movie.streams.empty()){
                const auto& h=movie.streams.front().header;
                put(info,28,word(h,20));put(info,32,word(h,24));put(info,36,word(h,32));
            }
            const auto label=utf16_bytes(u"AVI");std::copy(label.begin(),label.end(),info.begin()+44);
            memory.write(GuestAddress{call(1)},avi::View(info).first(std::min<std::size_t>(info.size(),static_cast<unsigned>(number(2)))));
            return done(static_cast<unsigned>(number(2))<info.size()?too_small:0);
        }
        const bool deleting=name=="DeleteStream";
        if(!deleting){
            if(!call(1))return done(bad_parameter);
            setptr(call(1),0);
        }
        unsigned index=0;
        if(name=="GetStream" || deleting){
            int skip=number(deleting?2:3);
            if(skip<0)return done(bad_parameter);
            const auto type=static_cast<unsigned>(number(deleting?1:2));
            while(index<movie.streams.size()){
                if((!type || word(movie.streams[index].header,0)==type) && !skip--)break;
                ++index;
            }
            if(index==movie.streams.size())return done(no_data);
            if(deleting){
                if(!disk.writable)return done(readonly);
                movie.streams.erase(movie.streams.begin()+index);
                std::erase_if(movie.packets,[&](auto& packet){
                    if(packet.stream==index){disk.payload-=packet.bytes.size();return true;}
                    if(packet.stream>index){
                        --packet.stream;
                        packet.id=(packet.id&0xffff0000U)|('0'+packet.stream/10)|(('0'+packet.stream%10)<<8);
                    }
                    return false;
                });
                for(auto& [pointer,other]:store.objects){
                    if(other.kind!=Kind::stream || other.disk!=self.disk || other.stream==UINT32_MAX)continue;
                    if(other.stream==index)other.stream=UINT32_MAX;
                    else if(other.stream>index)--other.stream;
                }
                update_main(movie);disk.dirty=true;return done(0);
            }
            for(auto& [pointer,other]:store.objects){
                if(other.kind==Kind::stream && other.disk==self.disk && other.stream==index){
                    ++other.refs;setptr(call(1),pointer);return done(0);
                }
            }
        }else{
            if(!disk.writable)return done(readonly);
            if(!call(2))return done(bad_parameter);
            if(movie.streams.size()>=100)return done(unsupported);
            index=static_cast<unsigned>(movie.streams.size());
            auto stream=unpack_stream(read_blob(2,204));
            if(!word(stream.header,20) || !word(stream.header,24))return done(bad_parameter);
            put(stream.header,32,0);movie.streams.push_back(std::move(stream));
        }
        auto hr=object(Kind::stream,ole_standard_guid(0x20021),call(1),self.disk,index);
        if(name=="CreateStream"){if(hr)movie.streams.pop_back();else disk.dirty=true;}
        return done(hr);
    }
    auto& stream=movie.streams[self.stream];
    auto& h=stream.header;
    const auto start=word(h,28),length=word(h,32),fixed=word(h,44);
    if(name=="Info"){
        if(!call(1))return done(bad_parameter);
            if(number(2)<0)return done(bad_size);
        const auto info=pack_stream(stream);
        memory.write(GuestAddress{call(1)},avi::View(info).first(std::min<std::size_t>(info.size(),static_cast<unsigned>(number(2)))));
        return done(static_cast<unsigned>(number(2))<info.size()?too_small:0);
    }
    if(name=="ReadFormat"){
        if(!call(3))return done(bad_parameter);
        const auto capacity=static_cast<std::int32_t>(memory.load(GuestAddress{call(3)},32));
        result(3,static_cast<unsigned>(stream.format.size()));
        if(!call(2) || capacity<=0)return done(0);
        const auto n=std::min<std::size_t>(static_cast<unsigned>(capacity),stream.format.size());
        memory.write(GuestAddress{call(2)},avi::View(stream.format).first(n));
        return done(n<stream.format.size()?too_small:0);
    }
    if(name=="Read"){
        result(5,0);result(6,0);
        if(number(1)<0 || call(1)<start || static_cast<unsigned>(number(1))-start>length)return done(no_data);
        if(number(4)<0 || number(2)<-1)return done(bad_parameter);
        auto skip=static_cast<unsigned>(number(1))-start;
        auto wanted=number(2)==-1?(fixed?static_cast<unsigned>(number(4))/fixed:1U):static_cast<unsigned>(number(2));
        if(!fixed)wanted=std::min(wanted,1U);
        wanted=std::min(wanted,length-skip);
        unsigned copied=0,samples=0;
        for(const auto& packet:movie.packets){
            if(!wanted)break;
            if(packet.stream!=self.stream)continue;
            unsigned count=fixed?static_cast<unsigned>(packet.bytes.size()/fixed):1;
            if(skip>=count){skip-=count;continue;}
            unsigned n=std::min(wanted,count-skip);
            if(call(3) && fixed)n=std::min(n,(static_cast<unsigned>(number(4))-copied)/fixed);
            const auto size=fixed?n*fixed:static_cast<unsigned>(packet.bytes.size());
            if(call(3)){
                if(size>static_cast<unsigned>(number(4))-copied)return done(too_small);
                if(size)memory.write(GuestAddress{call(3)+copied},avi::View(packet.bytes).subspan(skip*fixed,size));
            }
            copied+=size;samples+=n;wanted-=n;skip=0;
            if(!n)break;
        }
        result(5,copied);result(6,samples);return done(0);
    }
    if(name=="FindSample"){
        const auto flags=number(2);
        std::uint32_t pos=start,answer=UINT32_MAX;
        for(const auto& packet:movie.packets){
            if(packet.stream!=self.stream)continue;
            const auto n=fixed?static_cast<unsigned>(packet.bytes.size()/fixed):1;
            if((!(flags&0x10) || (packet.flags&0x10)) && (!(flags&0x40) || pos==start)){
                const auto target=number(1);
                const auto sample=fixed && target>=0 && static_cast<unsigned>(target)>=pos && static_cast<unsigned>(target)-pos<n?static_cast<unsigned>(target):pos;
                if((flags&8) || ((flags&4)?std::int64_t(sample)<=target:std::int64_t(sample)>=target)){
                    switch(flags&0xf000){case 0:answer=sample;break;case 0x1000:answer=static_cast<unsigned>(packet.bytes.size());break;case 0x3000:answer=fixed?fixed:1;break;default:break;}
                    if(!(flags&4) || (flags&8))break;
                }
            }
            pos+=n;
        }
        return done(answer);
    }
    if(!disk.writable)return done(readonly);
    if(name=="SetFormat"){
        if(!call(2) || number(3)<=0 || number(3)>static_cast<int>(memory_limit))return done(bad_size);
        const auto size=static_cast<unsigned>(number(3));
        if(disk.payload+size>memory_limit)return done(no_memory);
        auto format=read_blob(2,size);
        if(length && stream.format!=format)return done(unsupported);
        disk.payload+=size;disk.payload-=stream.format.size();stream.format=std::move(format);update_main(movie);disk.dirty=true;return done(0);
    }
    if(name=="Write"){
        result(6,0);result(7,0);
        const auto n=static_cast<unsigned>(number(2)),size=static_cast<unsigned>(number(4));
        if(number(2)<0 || number(4)<0 || (!call(3) && size) || stream.format.empty())return done(bad_parameter);
        if(number(1)!=-1 && static_cast<std::uint32_t>(number(1))!=std::uint64_t(start)+length)return done(unsupported);
        if(!n && !size)return done(0);
        if(fixed?std::uint64_t(n)*fixed!=static_cast<unsigned>(size):n!=1)return done(bad_size);
        if(disk.payload+size+sizeof(avi::Packet)>memory_limit || std::uint64_t(length)+n>INT32_MAX)return done(no_memory);
        const bool audio=word(h,0)==avi::audio;
        const bool raw=word(h,0)==avi::video && stream.format.size()>=20 && !word(stream.format,16);
        const auto id=avi::fourcc(static_cast<char>('0'+self.stream/10),static_cast<char>('0'+self.stream%10),audio?'w':'d',audio?'b':raw?'b':'c');
        movie.packets.push_back({self.stream,id,static_cast<unsigned>(number(5)),read_blob(3,size),disk.record});
        disk.payload+=size+sizeof(avi::Packet);put(h,32,length+n);put(h,36,std::max(word(h,36),static_cast<unsigned>(size)));
        update_main(movie);disk.dirty=true;result(6,n);result(7,size);return done(0);
    }
    return done(unsupported);
}
}
