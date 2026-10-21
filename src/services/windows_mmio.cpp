#include "x64arm/windows_services.hpp"
#include "x64arm/riff_chunk.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <array>
#include <bit>

namespace x64arm {
namespace {
constexpr std::uint32_t memory_proc=0x204d454d, disk_proc=0x20534f44, dirty=0x10000000;
constexpr std::uint64_t memory_limit=64U*1024U*1024U;
struct MmioError {std::uint32_t code;};
std::uint32_t open_error(std::uint32_t error){
    switch(error){case 2:return 257;case 3:return 267;case 5:return 268;case 8:return 258;case 32:return 269;default:return 272;}
}
void put(std::span<std::uint8_t> out,unsigned offset,std::uint32_t value){for(unsigned i=0;i<4;++i)out[offset+i]=static_cast<std::uint8_t>(value>>(i*8));}
std::uint64_t length(MultimediaFile& file){
    if(!file.native)return file.size;
    const auto info=file.native->information();if(info.error)throw MmioError{261};file.size=info.metadata.size;return file.size;
}
std::size_t read_at(MultimediaFile& file,GuestMemory& memory,std::uint64_t offset,std::span<std::uint8_t> bytes){
    if(file.mode==1)throw MmioError{261};
    const auto size=length(file);const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(),offset<size?size-offset:0));
    if(!count)return 0;bytes=bytes.first(count);
    if(file.native){const auto result=file.native->read(bytes,offset);if(result.error)throw MmioError{261};return static_cast<std::size_t>(result.bytes);}
    if(file.buffer)memory.read(GuestAddress{file.buffer+offset},bytes);
    else std::copy_n(file.bytes.begin()+static_cast<std::ptrdiff_t>(offset),count,bytes.begin());return count;
}
std::size_t write_at(MultimediaFile& file,GuestMemory& memory,std::uint64_t offset,std::span<const std::uint8_t> bytes){
    if(file.mode==0 || offset>INT32_MAX || bytes.size()>INT32_MAX-offset)throw MmioError{262};
    if(bytes.empty())return 0;
    if(file.native){const auto result=file.native->write(bytes,offset);if(result.error)throw MmioError{262};file.size=std::max(file.size,offset+result.bytes);return static_cast<std::size_t>(result.bytes);}
    const auto end=offset+bytes.size();
    if(end>file.capacity){
        if(file.buffer || !file.growth || end>memory_limit)throw MmioError{264};
        const auto capacity=std::min(memory_limit,((end+file.growth-1)/file.growth)*file.growth);
        file.bytes.resize(static_cast<std::size_t>(capacity));file.capacity=static_cast<std::uint32_t>(capacity);
    }
    if(file.buffer)memory.write(GuestAddress{file.buffer+offset},bytes);
    else std::copy(bytes.begin(),bytes.end(),file.bytes.begin()+static_cast<std::ptrdiff_t>(offset));
    file.size=std::max(file.size,end);return bytes.size();
}
}
std::optional<std::uint32_t> WindowsServices::invoke_mmio(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);const auto name=service.symbol;
    const bool opening=name=="mmioOpenA" || name=="mmioOpenW";
    const bool counted=name=="mmioRead" || name=="mmioWrite" || name=="mmioSeek";
    const auto finish=[&](std::uint64_t result){call.return_to(state,result);return std::optional<std::uint32_t>{};};
    const auto fail=[&](std::uint32_t error){
        if(opening){if(call(1))memory.store(GuestAddress{call(1)+(x86_?12U:16U)},32,error);return finish(0);}
        return finish(counted?UINT32_MAX:error);
    };
    try{
        if(opening){
            const auto flags=static_cast<std::uint32_t>(call(2)),mode=flags&3U,sharing=(flags>>4)&7U;
            const auto info=call(1);if(info)memory.check(GuestAddress{info},x86_?72U:100U,Access::Write);
            if(mode==3 || sharing>4 || (flags&~0x11073U))return fail(11);
            if(runtime_.multimedia_files.size()>=4096)return fail(271);
            const auto fourcc=info?memory.load(GuestAddress{info+4},32):0;
            if(info && memory.load(GuestAddress{info+8},x86_?32:64))return fail(259);
            MultimediaFile file;file.mode=mode;file.open_flags=flags;file.memory=fourcc==memory_proc;
            if(file.memory){
                if(call(0))return fail(11);
                const auto count=memory.load(GuestAddress{info+(x86_?20U:28U)},32);
                file.buffer=memory.load(GuestAddress{info+(x86_?24U:32U)},x86_?32:64);
                file.growth=static_cast<std::uint32_t>(memory.load(GuestAddress{info+(x86_?48U:72U)},32));
                if(count>memory_limit || (file.buffer && file.growth))return fail(11);
                file.capacity=static_cast<std::uint32_t>(count?count:8192);
                if(file.buffer){if(!count)return fail(11);memory.check(GuestAddress{file.buffer},file.capacity,mode?Access::Write:Access::Read);}
                else {file.bytes.resize(file.capacity);if(!file.growth)file.growth=8192;}
                file.size=flags&0x1000U?0:file.capacity;
            }else{
                if((fourcc && fourcc!=disk_proc) || !call(0))return fail(259);
                const auto path=name.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{call(0)})):read_ansi(memory,call(0));
                if(path.find('+')!=path.npos)return fail(259);
                if(!runtime_.network.external_io && (mode || (flags&0x1000U)))throw HostCallFault("比較模式不允許 MMIO 修改宿主檔案");
                const auto share=sharing==1?0U:sharing==2?1U:sharing==3?2U:3U;
                const auto opened=!filesystem_.writable() && !mode && !(flags&0x1000U)?filesystem_.open_reader(path,{}, {},share):
                    filesystem_.open_host(path,(mode!=1?0x80000000U:0U)|(mode?0x40000000U:0U),(flags&0x1000U)?2U:3U,0x80,share);
                if(opened.error)return fail(open_error(opened.error));
                if(opened.metadata.attributes&16U)return fail(272);
                file.native=opened.file;file.key=opened.key;file.size=opened.metadata.size;
            }
            // 直接存取緩衝區與記憶體檔案的底層資料分開，避免移動視窗覆蓋來源。
            if(file.memory || (flags&0x10000U)){
                file.io_capacity=file.memory?std::min(file.capacity,8192U):8192U;
                file.io_buffer=allocate_heap(memory,file.io_capacity);if(!file.io_buffer)return fail(258);
                file.io_owned=true;
            }else if(info){
                file.io_capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{info+(x86_?20U:28U)},32));
                file.io_buffer=memory.load(GuestAddress{info+(x86_?24U:32U)},x86_?32:64);
                if(file.io_capacity>memory_limit || (file.io_capacity && !file.io_buffer))return fail(11);
                if(file.io_capacity)memory.check(GuestAddress{file.io_buffer},file.io_capacity,Access::Write);
            }
            const auto handle=runtime_.allocate_wait_handle();runtime_.multimedia_files.emplace(handle,std::move(file));
            if(info)memory.store(GuestAddress{info+(x86_?12U:16U)},32,0);return finish(handle);
        }
        const auto found=runtime_.multimedia_files.find(call(0));if(found==runtime_.multimedia_files.end())return fail(5);
        auto& file=found->second;
        if(name=="mmioClose"){
            if(call(1))return fail(11);
            if(file.native && file.mode && file.native->flush())return fail(262);
            if(file.io_owned)(void)free_heap(memory,file.io_buffer,state);
            runtime_.multimedia_files.erase(found);return finish(0);
        }
        if(name=="mmioGetInfo" || name=="mmioSetInfo" || name=="mmioAdvance"){
            const auto info=call(1),flags=call(2);const bool advancing=name=="mmioAdvance",getting=name=="mmioGetInfo";
            if((advancing?flags>1:flags!=0) || (!advancing && !info))return fail(11);
            if(info)memory.check(GuestAddress{info},x86_?72:100,getting||advancing?Access::Write:Access::Read);
            if(advancing && !file.io_capacity)return fail(266);
            const std::uint8_t pointer_bits=x86_?32:64;const unsigned base_offset=x86_?24:32,pointer_bytes=x86_?4:8;
            if(!getting && info){
                const auto buffer=memory.load(GuestAddress{info+base_offset},pointer_bits);
                const auto next=memory.load(GuestAddress{info+base_offset+pointer_bytes},pointer_bits);
                const auto info_flags=memory.load(GuestAddress{info},32);
                if(buffer!=file.io_buffer || next<buffer || next-buffer>file.io_capacity ||
                   memory.load(GuestAddress{info+(x86_?20U:28U)},32)!=file.io_capacity ||
                   (info_flags&~dirty)!=file.open_flags)return fail(11);
                if(!file.io_ready && file.io_capacity)return fail(11);
                const auto consumed=next-buffer;
                if(!file.mode && consumed>file.io_valid)return fail(11);
                if(info_flags&dirty){
                    if(file.native && !runtime_.network.external_io)throw HostCallFault("比較模式不允許 MMIO 寫入宿主檔案");
                    std::vector<std::uint8_t> data(static_cast<std::size_t>(consumed));memory.read(GuestAddress{buffer},data);
                    if(write_at(file,memory,file.io_offset,data)!=data.size())return fail(262);
                    file.io_valid=std::max(file.io_valid,static_cast<std::uint32_t>(consumed));
                }
                file.io_next=consumed;file.position=file.io_offset+consumed;
            }else if(advancing && file.io_ready){file.position=file.io_offset+(flags?file.io_next:file.io_valid);}
            if(getting || advancing){
                file.io_offset=file.position;file.io_next=0;file.io_valid=0;
                if(file.io_capacity){
                    if((getting && file.mode!=1) || (advancing && flags==0)){
                        std::vector<std::uint8_t> data(file.io_capacity);
                        file.io_valid=static_cast<std::uint32_t>(read_at(file,memory,file.io_offset,data));
                        memory.write(GuestAddress{file.io_buffer},std::span(data).first(file.io_valid));
                    }
                    file.io_ready=true;
                }
                if(info){
                    memory.write(GuestAddress{info},std::vector<std::uint8_t>(x86_?72:100));
                    memory.store(GuestAddress{info},32,file.open_flags);
                    memory.store(GuestAddress{info+4},32,file.memory?memory_proc:disk_proc);
                    memory.store(GuestAddress{info+(x86_?20U:28U)},32,file.io_capacity);
                    memory.store(GuestAddress{info+base_offset},pointer_bits,file.io_buffer);
                    memory.store(GuestAddress{info+base_offset+pointer_bytes},pointer_bits,file.io_buffer);
                    memory.store(GuestAddress{info+base_offset+2*pointer_bytes},pointer_bits,file.io_buffer+file.io_valid);
                    memory.store(GuestAddress{info+base_offset+3*pointer_bytes},pointer_bits,file.io_buffer+file.io_capacity);
                    memory.store(GuestAddress{info+base_offset+4*pointer_bytes},32,file.io_offset);
                    memory.store(GuestAddress{info+base_offset+4*pointer_bytes+4},32,file.io_offset+file.io_valid);
                    memory.store(GuestAddress{info+(x86_?68U:92U)},pointer_bits,call(0));
                }
            }
            return finish(0);
        }
        file.io_ready=false;
        if(name=="mmioRead" || name=="mmioWrite"){
            const auto count=static_cast<std::uint32_t>(call(2));if(count>INT32_MAX)return fail(11);
            const bool writing=name=="mmioWrite";
            if((writing && !file.mode) || (!writing && file.mode==1))return fail(writing?262:261);
            const auto size=length(file),requested=writing?count:std::min<std::uint64_t>(count,file.position<size?size-file.position:0);
            if(writing && !runtime_.network.external_io && file.native)throw HostCallFault("比較模式不允許 MMIO 寫入宿主檔案");
            memory.check(GuestAddress{call(1)},static_cast<std::size_t>(requested),writing?Access::Read:Access::Write);
            std::array<std::uint8_t,65536> buffer{};std::uint64_t done=0;
            while(done<requested){
                auto part=std::span(buffer).first(static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(),requested-done)));
                if(writing)memory.read(GuestAddress{call(1)+done},part);
                const auto actual=writing?write_at(file,memory,file.position,part):read_at(file,memory,file.position,part);
                if(!writing && actual)memory.write(GuestAddress{call(1)+done},part.first(actual));
                file.position+=actual;done+=actual;if(actual<part.size())break;
            }
            return finish(done);
        }
        if(name=="mmioSeek"){
            const auto origin=static_cast<std::uint32_t>(call(2));if(origin>2)return fail(11);
            const auto offset=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(call(1)));
            const auto position=static_cast<std::int64_t>(origin==1?file.position:origin==2?length(file):0)+offset;
            if(position<0 || position>INT32_MAX)return fail(263);file.position=static_cast<std::uint64_t>(position);return finish(file.position);
        }
        if(!call(1))return fail(11);memory.check(GuestAddress{call(1)},20,Access::Write);
        const auto field=[&](unsigned offset){return static_cast<std::uint32_t>(memory.load(GuestAddress{call(1)+offset},32));};
        const auto save=[&](unsigned offset,std::uint64_t value){memory.store(GuestAddress{call(1)+offset},32,value);};
        if(name=="mmioDescend"){
            const auto flags=static_cast<std::uint32_t>(call(3));if(flags && flags!=0x10U && flags!=0x20U && flags!=0x40U)return fail(11);
            auto limit=length(file);if(call(2)){
                const auto parent=call(2),begin=memory.load(GuestAddress{parent+12},32),size=memory.load(GuestAddress{parent+4},32);
                if(file.position<begin)return fail(265);limit=std::min(limit,begin+size);
            }
            const auto id=flags==0x20U?riff::riff_id:flags==0x40U?riff::list_id:field(0),type=field(8);
            while(file.position<limit){
                std::array<std::uint8_t,8> header{};if(read_at(file,memory,file.position,header)!=8)return fail(265);
                const auto chunk=riff::chunk(header,file.position,limit);const bool container=chunk.id==riff::riff_id || chunk.id==riff::list_id;
                std::uint32_t form=0;if(container){if(chunk.size<4)return fail(265);std::array<std::uint8_t,4> bytes{};if(read_at(file,memory,chunk.data,bytes)!=4)return fail(265);form=riff::word(bytes);}
                if(!flags || (chunk.id==id && (flags==0x10U || form==type))){
                    if(chunk.data>UINT32_MAX)return fail(263);
                    save(0,chunk.id);save(4,chunk.size);save(8,form);save(12,chunk.data);save(16,0);file.position=chunk.data+(container?4:0);return finish(0);
                }
                file.position=chunk.next;
            }
            return fail(265);
        }
        if(file.native && !runtime_.network.external_io && (name=="mmioCreateChunk" || (field(16)&dirty)))throw HostCallFault("比較模式不允許 MMIO 更新宿主 chunk");
        if(name=="mmioCreateChunk"){
            const auto flags=static_cast<std::uint32_t>(call(2));if(flags && flags!=0x20U && flags!=0x40U)return fail(11);
            const auto id=flags==0x20U?riff::riff_id:flags==0x40U?riff::list_id:field(0);
            const auto count=id==riff::riff_id || id==riff::list_id?12U:8U;
            std::array<std::uint8_t,12> bytes{};put(bytes,0,id);put(bytes,4,field(4));put(bytes,8,field(8));
            if(write_at(file,memory,file.position,std::span(bytes).first(count))!=count)return fail(262);
            save(0,id);save(12,file.position+8);save(16,dirty);file.position+=count;return finish(0);
        }
        if(name=="mmioAscend"){
            if(call(2) || (field(16)&~dirty))return fail(11);
            const auto begin=std::uint64_t{field(12)};auto size=std::uint64_t{field(4)};
            if(field(16)&dirty){
                if(begin<8 || file.position<begin || file.position-begin>UINT32_MAX)return fail(263);
                size=file.position-begin;std::array<std::uint8_t,4> bytes{};put(bytes,0,static_cast<std::uint32_t>(size));
                if(write_at(file,memory,begin-4,bytes)!=4)return fail(262);
                if(size&1U){const std::array<std::uint8_t,1> pad{};if(write_at(file,memory,begin+size,pad)!=1)return fail(262);}
                save(4,size);save(16,0);
            }
            const auto next=begin+size+(size&1U);if(next>INT32_MAX)return fail(263);file.position=next;return finish(0);
        }
        return fail(8);
    }catch(const MmioError& error){return fail(error.code);}catch(const std::invalid_argument&){return fail(name=="mmioDescend"?265:11);}
    catch(const std::bad_alloc&){return fail(258);}catch(const MemoryFault&){if(opening){last_error_=998;return finish(0);}return fail(11);}
}
}
