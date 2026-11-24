#include "ansi_codepage.hpp"
#include "x64arm/windows_services.hpp"
#include "x64arm/audio_adpcm.hpp"
#include "x64arm/audio_ima_adpcm.hpp"
#include "x64arm/unicode.hpp"
#include "acm_g711.hpp"
#include "acm_gsm.hpp"
#include "acm_mpeg_audio.hpp"
#include "acm_volume_filter.hpp"
#include "acm_choose.hpp"
#include "acm_registered_driver.hpp"
#include "dialog_template.hpp"
#include "service_callback_task.hpp"
#include "wave_acm_stream.hpp"
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#endif
namespace x64arm {
namespace {
struct AcmError {std::uint32_t code;};
struct AcmCall : WindowsCall {
    using WindowsCall::WindowsCall;
    std::optional<std::array<std::uint64_t,5>> message;
    std::uint64_t operator()(unsigned index)const{
        return message?message->at(index):WindowsCall::operator()(index);
    }
};
std::uint32_t read_volume(GuestMemory& memory,std::uint64_t address){
    if(!address || memory.load(GuestAddress{address},32)<acm::volume_size)throw AcmError{11};
    if(memory.load(GuestAddress{address+4},32)!=acm::volume_tag)throw AcmError{512};
    if(memory.load(GuestAddress{address+8},32))throw AcmError{10};
    return static_cast<std::uint32_t>(memory.load(GuestAddress{address+32},32));
}
struct Format {
    unsigned tag{},channels{},rate{},average{},align{},bits{};
    std::optional<audio::AdpcmFormat> adpcm;
    unsigned mpeg_layer{};
    std::optional<audio::ImaAdpcmFormat> ima{};
    unsigned block_frames()const{
        if(adpcm)return adpcm->samples_per_block;
        if(tag==acm::gsm_tag)return acm::gsm_samples;
        return ima?ima->samples_per_block:0;
    }
};
bool is_adpcm_format(unsigned tag){return tag==2 || tag==audio::ima_adpcm_tag;}
Format read_format(GuestMemory& memory,std::uint64_t pointer,unsigned available=UINT32_MAX){
    if(!pointer || available<16)throw AcmError{11};
    const auto get=[&](unsigned at,std::uint8_t bits){return static_cast<unsigned>(memory.load(GuestAddress{pointer+at},bits));};
    Format f{get(0,16),get(2,16),get(4,32),get(8,32),get(12,16),get(14,16),{}};
    if(!f.channels || f.channels>2 || f.rate<1000 || f.rate>384000 || !f.align || !f.average)throw AcmError{512};
    if(f.tag==1 || f.tag==3){if((f.tag==3 && f.bits!=32) || (f.bits!=8 && f.bits!=16 && f.bits!=24 && f.bits!=32) || f.align!=f.channels*f.bits/8 || f.average!=f.rate*f.align)throw AcmError{512};}
    else if(f.tag==2){
        if(available<22)throw AcmError{11};
        const auto extra=get(16,16);
        if(extra>available-18U)throw AcmError{11};
        std::vector<std::uint8_t> bytes(18U+extra);memory.read(GuestAddress{pointer},bytes);
        f.adpcm=audio::parse_adpcm_format(bytes);if(!f.adpcm)throw AcmError{512};
    }else if(f.tag==audio::ima_adpcm_tag){
        if(available<20)throw AcmError{11};
        const auto extra=get(16,16);
        if(extra<2 || extra>available-18U || f.bits!=4)throw AcmError{512};
        f.ima=audio::ImaAdpcmFormat{f.channels,f.align,get(18,16)};
        if(!f.ima->valid())throw AcmError{512};
    }else if(f.tag==acm::gsm_tag){
        if(available<20)throw AcmError{11};
        const auto extra=get(16,16);
        if(extra<2 || extra>available-18U || f.channels!=1 || f.bits || f.align!=acm::gsm_block_bytes ||
           f.average!=f.rate*acm::gsm_block_bytes/acm::gsm_samples || get(18,16)!=acm::gsm_samples)
            throw AcmError{512};
    }else if(acm::is_g711(f.tag)){
        if(f.bits!=8 || f.align!=f.channels || f.average!=f.rate*f.channels)throw AcmError{512};
    }else if(f.tag==acm::mpeg){
        if(available<40)throw AcmError{11};
        const auto extra=get(16,16);
        if(extra<22 || extra>available-18U)throw AcmError{512};
        const auto layer=get(18,16);
        if(layer!=1 && layer!=2 && layer!=4)throw AcmError{512};
        f.mpeg_layer=layer==4?3U:layer;
    }else if(f.tag==acm::mp3){
        f.mpeg_layer=3;
    }else throw AcmError{512};
    return f;
}
unsigned builtin_codec_for(unsigned source,unsigned destination){
    if(source==2 || destination==2)return acm::ms_adpcm_codec;
    if(source==audio::ima_adpcm_tag || destination==audio::ima_adpcm_tag)return acm::ima_codec;
    if(source==acm::gsm_tag || destination==acm::gsm_tag)return acm::gsm_codec;
    if(acm::is_mpeg_audio(source) || acm::is_mpeg_audio(destination))return acm::mpeg_codec;
    if(acm::is_g711(source) || acm::is_g711(destination))return acm::g711_codec;
    return acm::pcm_codec;
}
bool builtin_conversion_source(unsigned tag){return tag==1 || tag==3 || tag==acm::gsm_tag ||
    is_adpcm_format(tag) || acm::is_mpeg_audio(tag) || acm::is_g711(tag);}
bool compatible_codec_formats(const Format& source,const Format& destination){
    const bool same_layout=source.rate==destination.rate && source.channels==destination.channels;
    const auto pcm16=[](const Format& format){return format.tag==1 && format.bits==16;};
    if(source.tag==acm::gsm_tag || destination.tag==acm::gsm_tag){
        return same_layout && source.channels==1 &&
            ((source.tag==acm::gsm_tag && pcm16(destination)) || (destination.tag==acm::gsm_tag && pcm16(source)));
    }
    if(acm::is_g711(source.tag) || acm::is_g711(destination.tag))
        return same_layout && (acm::is_g711(source.tag) || pcm16(source)) &&
            (acm::is_g711(destination.tag) || pcm16(destination));
    if(is_adpcm_format(destination.tag))return same_layout && pcm16(source);
    return destination.tag==1 || destination.tag==3;
}
std::vector<std::uint8_t> standard_adpcm(unsigned rate,unsigned channels,unsigned tag){
    const auto align=(rate<=11025?256U:rate<=22050?512U:1024U)*channels;
    const bool ima=tag==audio::ima_adpcm_tag;
    const auto samples=ima?1+(align-4*channels)*2/channels:2+(align-7*channels)*2/channels;
    std::vector<std::uint8_t> bytes(ima?20U:22U+audio::adpcm_coefficients.size()*4);
    const auto store=[&](unsigned offset,unsigned value,unsigned count){
        for(unsigned i=0;i<count;++i)bytes[offset+i]=static_cast<std::uint8_t>(value>>(i*8));
    };
    store(0,tag,2);store(2,channels,2);store(4,rate,4);store(8,rate*align/samples,4);
    store(12,align,2);store(14,4,2);store(16,static_cast<unsigned>(bytes.size()-18),2);
    store(18,samples,2);
    if(ima)return bytes;
    store(20,audio::adpcm_coefficients.size(),2);
    for(unsigned i=0;i<audio::adpcm_coefficients.size();++i){
        store(22+i*4,static_cast<std::uint16_t>(audio::adpcm_coefficients[i][0]),2);
        store(24+i*4,static_cast<std::uint16_t>(audio::adpcm_coefficients[i][1]),2);
    }
    return bytes;
}
std::array<std::uint8_t,20> standard_gsm(unsigned rate){
    std::array<std::uint8_t,20> bytes{};
    const auto store=[&](unsigned offset,unsigned value,unsigned size){
        for(unsigned i=0;i<size;++i)bytes[offset+i]=static_cast<std::uint8_t>(value>>(8*i));
    };
    store(0,acm::gsm_tag,2);store(2,1,2);store(4,rate,4);
    store(8,rate*acm::gsm_block_bytes/acm::gsm_samples,4);
    store(12,acm::gsm_block_bytes,2);store(16,2,2);store(18,acm::gsm_samples,2);
    return bytes;
}
#ifdef __APPLE__
AudioStreamBasicDescription description(const Format& f){AudioStreamBasicDescription d{};d.mSampleRate=f.rate;d.mChannelsPerFrame=f.channels;
    if(f.mpeg_layer){
        constexpr AudioFormatID formats[]{kAudioFormatMPEGLayer1,kAudioFormatMPEGLayer2,kAudioFormatMPEGLayer3};
        d.mFormatID=formats[f.mpeg_layer-1];
        d.mFramesPerPacket=f.mpeg_layer==1?384U:f.mpeg_layer==3 && f.rate<32000?576U:1152U;
    }
    else{const auto bits=f.block_frames()?16U:f.bits;d.mFormatID=kAudioFormatLinearPCM;d.mFormatFlags=kAudioFormatFlagIsPacked|(f.tag==3?kAudioFormatFlagIsFloat:bits>8?kAudioFormatFlagIsSignedInteger:0);d.mBitsPerChannel=bits;d.mFramesPerPacket=1;d.mBytesPerFrame=d.mBytesPerPacket=f.block_frames()?f.channels*2:f.align;}return d;}
#endif
struct Stream {
    Format source,destination;std::map<std::uint64_t,acm::DriverStream::Header> headers;std::uint64_t driver{};
    std::span<const std::uint8_t> input;std::vector<std::uint8_t> decoded;std::size_t cursor{},decoded_offset{};
    std::uint64_t codec_instance{};
    bool ending{},block_align{},direct_pcm{},copy_pcm{},g711{};
    std::optional<std::uint32_t> volume;
    std::unique_ptr<acm::GsmWav49> gsm;
#ifdef __APPLE__
    AudioConverterRef converter{};
    std::vector<std::uint8_t> retained,mpeg_partial;
    AudioStreamPacketDescription packet{};
#endif
    Stream(Format s,Format d,std::optional<std::uint32_t> gain):source(std::move(s)),destination(std::move(d)),volume(gain){
        if(!compatible_codec_formats(source,destination))throw AcmError{512};
        if(source.tag==acm::gsm_tag || destination.tag==acm::gsm_tag){
            if(volume)throw AcmError{512};
            gsm=std::make_unique<acm::GsmWav49>();
            direct_pcm=source.tag==acm::gsm_tag;
            return;
        }
        g711=acm::is_g711(source.tag) || acm::is_g711(destination.tag);
        if(g711 || destination.block_frames()){
            if(volume)throw AcmError{512};
            return;
        }
        if(volume){
            if(source.tag!=destination.tag || source.channels!=destination.channels ||
               source.rate!=destination.rate || source.bits!=destination.bits)throw AcmError{512};
            return;
        }
        direct_pcm=source.block_frames() && destination.tag==1 && destination.bits==16 &&
            source.channels==destination.channels && source.rate==destination.rate;
        copy_pcm=(source.tag==1 || source.tag==3) && source.tag==destination.tag &&
            source.bits==destination.bits && source.channels==destination.channels && source.rate==destination.rate;
        if(direct_pcm || copy_pcm)return;
#ifdef __APPLE__
        auto in=description(source),out=description(destination);if(AudioConverterNew(&in,&out,&converter)!=noErr)throw AcmError{512};
#else
        throw AcmError{512};
#endif
    }
    ~Stream(){
#ifdef __APPLE__
        if(converter)AudioConverterDispose(converter);
#endif
    }
    void reset(){
        if(gsm)gsm->reset();
#ifdef __APPLE__
        if(converter && AudioConverterReset(converter)!=noErr)throw AcmError{512};
        mpeg_partial.clear();
#endif
        input={};decoded.clear();decoded_offset=cursor=0;
    }
    // 每次只保留尚未交付的單一區塊，分開計算客體來源與目的緩衝區的使用量。
    std::span<const std::uint8_t> read_pcm_block(unsigned frames){
        if(!frames)return {};
        if(decoded_offset==decoded.size()){
            const auto bytes=input.subspan(cursor);
            if(bytes.empty() || ((block_align || gsm) && bytes.size()<source.align))return {};
            const auto length=std::min<std::size_t>(bytes.size(),source.align);
            if(gsm)decoded=gsm->decode(bytes.first(length));
            else if(source.ima)decoded=audio::decode_ima_adpcm_block(bytes.first(length),*source.ima);
            else{
                decoded=audio::decode_adpcm(bytes.first(length),*source.adpcm);
                // XAudio 的共用 MS ADPCM 解碼器會補齊尾區塊，這裡只交付來源實際包含的樣本。
                const auto available=2+(length-7*source.channels)*2/source.channels;
                decoded.resize(available*source.channels*2);
            }
            decoded_offset=0;cursor+=length;
        }
        const auto length=std::min<std::size_t>(decoded.size()-decoded_offset,std::size_t{frames}*source.channels*2);
        const auto bytes=std::span<const std::uint8_t>(decoded).subspan(decoded_offset,length);decoded_offset+=length;return bytes;
    }
    std::uint64_t size(unsigned bytes,bool from_destination)const{
        const auto ceil_div=[](std::uint64_t n,std::uint64_t d){return (n+d-1)/d;};
        if(g711){
            const auto& in=from_destination?destination:source;
            const auto& out=from_destination?source:destination;
            return ceil_div(bytes,in.align)*out.align;
        }
        if(const auto destination_frames=destination.block_frames()){
            const auto block=std::uint64_t{destination_frames}*source.align;
            return from_destination?ceil_div(bytes,destination.align)*block:
                ceil_div(bytes,block)*destination.align;
        }
        if(const auto source_frames=source.block_frames()){
            if(from_destination){const auto frames=ceil_div(ceil_div(bytes,destination.align)*source.rate,destination.rate);return ceil_div(frames,source_frames)*source.align;}
            auto frames=ceil_div(ceil_div(bytes,source.align)*source_frames*destination.rate,source.rate);
            // 重採樣器可能保留濾波尾端；同採樣率的區塊換算保持精確。
            if(source.rate!=destination.rate)frames+=256;
            return frames*destination.align;
        }
        const auto& in=from_destination?destination:source;const auto& out=from_destination?source:destination;
        auto bytes_out=ceil_div(std::uint64_t{bytes}*out.average,in.average);
        if(in.mpeg_layer)bytes_out+=out.align*2304U;
        return std::max<std::uint64_t>(out.align,ceil_div(bytes_out,out.align)*out.align);
    }
#ifdef __APPLE__
    OSStatus supply_mpeg(UInt32* count,AudioBufferList* buffers,AudioStreamPacketDescription** packets){
        const auto bytes=input.subspan(cursor);
        const auto total=bytes.size()+mpeg_partial.size();
        const auto incomplete=[&]()->OSStatus{
            *count=0;
            if(!block_align && !ending){
                mpeg_partial.insert(mpeg_partial.end(),bytes.begin(),bytes.end());
                cursor+=bytes.size();
            }
            return ending?noErr:1;
        };
        try{
            if(total<4)return incomplete();
            std::array<std::uint8_t,4> header{};
            const auto pending_header=std::min(header.size(),mpeg_partial.size());
            std::copy_n(mpeg_partial.begin(),pending_header,header.begin());
            std::copy_n(bytes.begin(),header.size()-pending_header,header.begin()+static_cast<std::ptrdiff_t>(pending_header));
            const auto frame=acm::mpeg_frame(header);
            if(!frame || frame->layer!=source.mpeg_layer || frame->rate!=source.rate || frame->channels!=source.channels ||
               frame->bytes<mpeg_partial.size()){
                *count=0;
                return kAudioConverterErr_InvalidInputSize;
            }
            if(total<frame->bytes)return incomplete();
            const auto consumed=frame->bytes-mpeg_partial.size();
            retained.assign(mpeg_partial.begin(),mpeg_partial.end());
            retained.insert(retained.end(),bytes.begin(),bytes.begin()+static_cast<std::ptrdiff_t>(consumed));
            mpeg_partial.clear();
            cursor+=consumed;
            packet={0,frame->frames,frame->bytes};
            *count=1;
            if(packets)*packets=&packet;
            buffers->mNumberBuffers=1;
            buffers->mBuffers[0]={source.channels,frame->bytes,retained.data()};
            return noErr;
        }catch(const std::bad_alloc&){
            *count=0;
            return -108;
        }
    }
    static OSStatus supply(AudioConverterRef,UInt32* count,AudioBufferList* buffers,AudioStreamPacketDescription** packets,void* context){
        auto& self=*static_cast<Stream*>(context);const auto bytes=self.input.subspan(self.cursor);unsigned length;
        if(self.source.block_frames()){
            try{const auto pcm=self.read_pcm_block(*count);*count=static_cast<UInt32>(pcm.size()/(self.source.channels*2));if(packets)*packets=nullptr;
                if(pcm.empty())return self.ending?noErr:1;
                self.retained.assign(pcm.begin(),pcm.end());buffers->mNumberBuffers=1;buffers->mBuffers[0]={self.source.channels,static_cast<UInt32>(pcm.size()),self.retained.data()};return noErr;
            }catch(const std::bad_alloc&){*count=0;return -108;}catch(const std::invalid_argument&){*count=0;return kAudioConverterErr_InvalidInputSize;}
        }
        if(self.source.mpeg_layer)return self.supply_mpeg(count,buffers,packets);
        *count=std::min(*count,static_cast<UInt32>(bytes.size()/self.source.align));
        length=*count*self.source.align;
        if(packets)*packets=nullptr;
        if(!length){*count=0;return self.ending?noErr:1;}
        // Core Audio 可保留本次 callback 的資料，直到下次索取；不可指向呼叫端的短命 staging。
        try{self.retained.assign(bytes.begin(),bytes.begin()+length);}catch(const std::bad_alloc&){*count=0;return -108;}
        buffers->mNumberBuffers=1;buffers->mBuffers[0]={self.source.channels,length,self.retained.data()};self.cursor+=length;return noErr;
    }
#endif
    std::pair<unsigned,unsigned> convert(std::span<const std::uint8_t> bytes,std::span<std::uint8_t> output,unsigned flags){
        if(g711){
            const auto frames=static_cast<unsigned>(std::min(bytes.size()/source.align,output.size()/destination.align));
            const auto consumed=frames*source.align,written=frames*destination.align;
            if(source.tag==destination.tag)std::copy_n(bytes.begin(),consumed,output.begin());
            else acm::convert_g711(bytes.first(consumed),output.first(written),source.tag,destination.tag);
            return {consumed,written};
        }
        if(const auto destination_frames=destination.block_frames()){
            const auto block=std::size_t{destination_frames}*source.align;
            std::size_t consumed=0,written=0;
            while(bytes.size()-consumed>=source.align && output.size()-written>=destination.align){
                const auto available=std::min(block,(bytes.size()-consumed)/source.align*source.align);
                if(available<block && ((flags&4) || (!(flags&0x20) && !gsm)))break;
                const auto input_block=bytes.subspan(consumed,available);
                const auto output_block=output.subspan(written,destination.align);
                if(gsm)gsm->encode(input_block,output_block);
                else if(destination.ima)audio::encode_ima_adpcm_block(input_block,output_block,*destination.ima);
                else audio::encode_adpcm_block(input_block,output_block,*destination.adpcm);
                consumed+=available;
                written+=destination.align;
            }
            return {static_cast<unsigned>(consumed),static_cast<unsigned>(written)};
        }
        if(copy_pcm){
            const auto size=static_cast<unsigned>(std::min(bytes.size(),output.size())/source.align*source.align);
            std::copy_n(bytes.begin(),size,output.begin());
            return {size,size};
        }
        if(volume){
            const auto size=static_cast<unsigned>(std::min(bytes.size(),output.size())/source.align*source.align);
            acm::apply_volume(bytes.first(size),output.first(size),source.bits,source.tag==3,*volume);
            return {size,size};
        }
        input=bytes;cursor=0;ending=(flags&0x20)!=0;block_align=(flags&4)!=0;unsigned written=0;
        try{
            if(direct_pcm){
                while(output.size()-written>=destination.align){const auto pcm=read_pcm_block(static_cast<unsigned>((output.size()-written)/destination.align));if(pcm.empty())break;
                    std::copy(pcm.begin(),pcm.end(),output.begin()+written);written+=static_cast<unsigned>(pcm.size());}
            }else{
#ifdef __APPLE__
                UInt32 frames=static_cast<UInt32>(output.size()/destination.align);
                if(frames){AudioBufferList buffer{1,{{destination.channels,frames*destination.align,output.data()}}};
                    const auto result=AudioConverterFillComplexBuffer(converter,supply,this,&frames,&buffer,nullptr);if(result==-108)throw std::bad_alloc{};if(result!=noErr && result!=1)throw AcmError{512};written=frames*destination.align;}
#endif
            }
        }catch(...){input={};throw;}
        input={};return {static_cast<unsigned>(cursor),written};
    }
};
}
struct wave::AcmStream::Data {
    Stream stream;
    bool capture;
    Data(Format source,Format destination,bool input):stream(std::move(source),std::move(destination),{}),capture(input){}
    const Format& wire()const{return capture?stream.destination:stream.source;}
    const Format& pcm()const{return capture?stream.source:stream.destination;}
};
wave::AcmStream::AcmStream(std::unique_ptr<Data> data):data_(std::move(data)){}
wave::AcmStream::~AcmStream()=default;
PcmWaveFormat wave::AcmStream::pcm_format()const{
    const auto& pcm=data_->pcm();
    return {pcm.channels,pcm.rate,pcm.bits,pcm.align,false};
}
unsigned wave::AcmStream::wire_alignment()const{return data_->wire().align;}
unsigned wave::AcmStream::packet_frames()const{
    const auto frames=data_->wire().block_frames();
    return frames?frames:1;
}
std::uint64_t wave::AcmStream::wire_bytes(std::uint64_t frames)const{
    const auto& wire=data_->wire();
    if(wire.mpeg_layer)return frames/wire.rate*wire.average+(frames%wire.rate)*wire.average/wire.rate;
    return frames/packet_frames()*wire.align;
}
void wave::AcmStream::reset(){
    try{data_->stream.reset();}
    catch(const AcmError&){throw std::invalid_argument("Wave Mapper 無法重設轉換狀態");}
}
std::vector<std::uint8_t> wave::AcmStream::convert(std::span<const std::uint8_t> input,std::size_t limit,bool end){
    if(input.empty())return {};
    auto& stream=data_->stream;
    const auto capacity=stream.size(static_cast<unsigned>(input.size()),false);
    if(capacity>limit)throw std::length_error("Wave Mapper 的轉換緩衝區已滿");
    std::vector<std::uint8_t> output(static_cast<std::size_t>(capacity));
    try{
        const unsigned flags=end?0x20U:(!data_->wire().mpeg_layer?4U:0U);
        const auto [consumed,written]=stream.convert(input,output,flags);
        if(consumed!=input.size())throw std::invalid_argument("Wave Mapper 沒有取得完整音訊區塊");
        output.resize(written);
        return output;
    }catch(const AcmError&){throw std::invalid_argument("Wave Mapper 轉換失敗");}
}
// streams：HACMSTREAM；drivers：acmDriverOpen 發出的 HACMDRIVER→內建驅動程式索引；order／enabled：acmDriverPriority 調整的列舉順序與啟用狀態。
struct AcmState {std::map<std::uint64_t,std::shared_ptr<Stream>> streams;std::map<std::string_view,std::uint64_t> trace_calls;
    std::map<std::uint64_t,unsigned> drivers;
    struct CodecSession {std::uint64_t endpoint;unsigned codec;};
    std::map<std::uint64_t,CodecSession> codec_sessions;
    std::vector<std::uint64_t> order;
    std::array<bool,acm::builtin_codec_modules.size()> enabled{},installed{};
    std::uint64_t next_driver=0x6c100000;
    std::map<std::uint64_t,std::shared_ptr<acm::ChoiceDialog>> dialogs;
    std::map<std::uint64_t,std::shared_ptr<acm::RegisteredDriver>> registered;
    std::map<std::uint64_t,std::shared_ptr<acm::DriverInstance>> registered_instances;
    std::map<std::uint64_t,std::shared_ptr<acm::DriverStream>> registered_streams;
    AcmState(){
        enabled.fill(true);
        installed.fill(true);
        for(unsigned i=0;i<enabled.size();++i)order.push_back(acm::builtin_driver_base+i);
    }
};
std::unique_ptr<wave::AcmStream> wave::AcmStream::open(GuestMemory& memory,std::uint64_t address,bool capture,const AcmState* catalog){
    try{
        auto wire=read_format(memory,address);
        if(wire.tag==1 || wire.tag==3)return {};
        const auto codec=builtin_codec_for(wire.tag,1);
        if(catalog && (!catalog->installed[codec] || !catalog->enabled[codec]))return {};
        const auto align=2*wire.channels;
        Format pcm{1,wire.channels,wire.rate,wire.rate*align,align,16,{}};
        auto data=capture?std::make_unique<Data>(std::move(pcm),std::move(wire),true):
            std::make_unique<Data>(std::move(wire),std::move(pcm),false);
        return std::unique_ptr<AcmStream>(new AcmStream(std::move(data)));
    }catch(const AcmError&){return {};}
}
std::optional<std::uint32_t> WindowsServices::invoke_acm(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    AcmCall arg(state,memory,service);auto name=service.symbol;const auto d=[&](unsigned n){return static_cast<unsigned>(arg(n));};const std::uint8_t pointer_bits=x86_?32:64;std::uint32_t result=0;
    std::optional<unsigned> codec_driver;
    unsigned source_format_capacity=UINT32_MAX;
    const acm::StreamHeaderAbi codec_header{x86_?4U:8U};
    std::shared_ptr<Stream> traced;unsigned consumed_bytes=0,written_bytes=0;
    try{
#include "windows_acm_choose_proc.inc"
        if(name=="acmGetVersion"){arg.return_to(state,0x04000000);return {};}
#include "windows_acm_registered.inc"
#include "windows_acm_codec_driver.inc"
#include "windows_acm_catalog.inc"
        // had 可為 NULL 或 acmDriverOpen 的驅動程式；指定時來源格式須屬於該驅動程式。
        const auto driver_accepts=[&](std::uint64_t had,unsigned selected){
            if(codec_driver){
                if(*codec_driver!=selected)throw AcmError{512};
                return;
            }
            if(!had){
                if(acm_ && !acm_->enabled[selected])throw AcmError{512};
                return;
            }
            if(!acm_ || !acm_->drivers.contains(had))throw AcmError{5};
            if(acm_->drivers.at(had)!=selected)throw AcmError{512};
        };
        if(name=="acmFormatSuggest"){
            const auto source=read_format(memory,arg(1),source_format_capacity);
            if(!arg(2) || d(3)<16)throw AcmError{11};
            if(d(4)&~0xf0000U)throw AcmError{10};
            const auto get=[&](unsigned at,std::uint8_t bits){return static_cast<unsigned>(memory.load(GuestAddress{arg(2)+at},bits));};
            auto preferred=codec_driver;
            if(!preferred && arg(0) && acm_){
                if(const auto found=acm_->drivers.find(arg(0));found!=acm_->drivers.end())preferred=found->second;
            }
            auto recommendation=1U;
            if(source.tag==1){
                if(preferred==acm::ms_adpcm_codec)recommendation=2;
                if(preferred==acm::ima_codec)recommendation=audio::ima_adpcm_tag;
                if(preferred==acm::gsm_codec)recommendation=acm::gsm_tag;
                if(preferred==acm::g711_codec)recommendation=acm::a_law;
            }
            const auto tag=d(4)&0x10000?get(0,16):recommendation;
            const auto channels=d(4)&0x20000?get(2,16):source.channels,rate=d(4)&0x40000?get(4,32):source.rate;
            const auto bits=d(4)&0x80000?get(14,16):is_adpcm_format(tag)?4U:tag==acm::gsm_tag?0U:
                tag==3?32U:acm::is_g711(tag)?8U:16U;
            const auto selected=builtin_codec_for(source.tag,tag);
            driver_accepts(arg(0),selected);
            if(is_adpcm_format(tag)){
                if(source.tag!=1 || source.bits!=16 || channels!=source.channels || rate!=source.rate ||
                   bits!=4)throw AcmError{512};
                const auto wave=standard_adpcm(rate,channels,tag);
                if(d(3)<wave.size())throw AcmError{512};
                memory.write(GuestAddress{arg(2)},wave);
                arg.return_to(state,0);
                return {};
            }
            if(tag==acm::gsm_tag){
                if(source.tag!=1 || source.bits!=16 || source.channels!=1 || channels!=1 ||
                   rate!=source.rate || bits || d(3)<20)throw AcmError{512};
                memory.write(GuestAddress{arg(2)},standard_gsm(rate));
                arg.return_to(state,0);
                return {};
            }
            if(source.tag==acm::gsm_tag &&
               (tag!=1 || bits!=16 || channels!=1 || rate!=source.rate))throw AcmError{512};
            if(selected==acm::g711_codec){
                if(source.rate!=rate || source.channels!=channels ||
                   (source.tag==1 && source.bits!=16) || (!acm::is_g711(source.tag) && source.tag!=1) ||
                   (tag==1?bits!=16:!acm::is_g711(tag) || bits!=8 || d(3)<18))throw AcmError{512};
            }else if(tag!=1 && tag!=3)throw AcmError{512};
            if(!channels || channels>2 || rate<1000 || rate>384000 ||
               (tag==3 && (bits!=32 || d(3)<18)) ||
               (bits!=8 && bits!=16 && bits!=24 && bits!=32))throw AcmError{512};
            const auto align=channels*bits/8;memory.check(GuestAddress{arg(2)},std::min(d(3),18U),Access::Write);
            memory.store(GuestAddress{arg(2)},16,tag);memory.store(GuestAddress{arg(2)+2},16,channels);memory.store(GuestAddress{arg(2)+4},32,rate);memory.store(GuestAddress{arg(2)+8},32,rate*align);memory.store(GuestAddress{arg(2)+12},16,align);memory.store(GuestAddress{arg(2)+14},16,bits);if(d(3)>=18)memory.store(GuestAddress{arg(2)+16},16,0);
        }else if(name=="acmStreamOpen"){
            if(d(7)&~7U)throw AcmError{10};if(arg(1) && (!acm_ || !acm_->drivers.contains(arg(1))))throw AcmError{5};if(d(7)&2)throw AcmError{512};if(arg(5) || arg(6) || (!(d(7)&1) && !arg(0)))throw AcmError{11};if(arg(0))memory.store(GuestAddress{arg(0)},pointer_bits,0);
            const auto gain=arg(4)?std::optional{read_volume(memory,arg(4))}:std::nullopt;
            auto stream=std::make_shared<Stream>(read_format(memory,arg(2)),read_format(memory,arg(3)),gain);traced=stream;
            driver_accepts(arg(1),builtin_codec_for(stream->source.tag,stream->destination.tag));
            stream->driver=arg(1);
            if(!(d(7)&1)){
                if(!acm_)acm_=std::make_shared<AcmState>();
                if(acm_->registered_streams.size()+acm_->streams.size()>=256)throw AcmError{4};
                memory.check(GuestAddress{arg(0)},pointer_bits/8,Access::Write);
                const auto handle=runtime_.allocate_wait_handle();
                acm_->streams.emplace(handle,std::move(stream));
                memory.store(GuestAddress{arg(0)},pointer_bits,handle);
            }
        }else{
            if(name=="acmStreamSize" && arg(2))memory.store(GuestAddress{arg(2)},32,0);if(!acm_ || !acm_->streams.contains(arg(0)))throw AcmError{5};auto stream=acm_->streams.at(arg(0));traced=stream;
            if(name=="acmStreamSize"){
                if(!arg(2) || !d(1))throw AcmError{11};if(d(3)>1)throw AcmError{10};const auto size=stream->size(d(1),d(3)!=0);if(size>UINT32_MAX)throw AcmError{512};memory.store(GuestAddress{arg(2)},32,size);
            }else if(name=="acmStreamClose"){if(d(1))throw AcmError{10};if(!stream->headers.empty())throw AcmError{513};acm_->streams.erase(arg(0));}
            else if(name=="acmStreamReset"){if(d(1))throw AcmError{10};stream->reset();}
            else if(name=="acmStreamMessage")throw AcmError{8};
            else{
                const auto address=arg(1);if(!address)throw AcmError{11};const auto size=codec_driver?codec_header.size():x86_?84U:104U;
                const auto source_pointer=codec_header.source(),source_length=codec_header.source_length();
                const auto destination_pointer=codec_header.destination(),destination_length=codec_header.destination_length();
                if(memory.load(GuestAddress{address},32)<size)throw AcmError{11};memory.check(GuestAddress{address},size,Access::Write);const auto flags=static_cast<unsigned>(memory.load(GuestAddress{address+4},32));
                const auto src=memory.load(GuestAddress{address+source_pointer},pointer_bits),dst=memory.load(GuestAddress{address+destination_pointer},pointer_bits);const auto src_size=static_cast<unsigned>(memory.load(GuestAddress{address+source_length},32)),dst_size=static_cast<unsigned>(memory.load(GuestAddress{address+destination_length},32));
                if(name=="acmStreamPrepareHeader"){
                    if(d(2))throw AcmError{10};
                    if(!stream->headers.contains(address)){
                        if(src_size>64U*1024U*1024U || dst_size>64U*1024U*1024U ||
                           (!src && src_size) || !dst || !dst_size)throw AcmError{11};
                        memory.check(GuestAddress{src},src_size,Access::Read);
                        memory.check(GuestAddress{dst},dst_size,Access::Write);
                        stream->headers.emplace(address,acm::DriverStream::Header{src,dst,src_size,dst_size});
                        memory.store(GuestAddress{address+4},32,(flags&~acm::status_done)|acm::status_prepared);
                    }
                }
                else if(name=="acmStreamUnprepareHeader"){
                    if(d(2))throw AcmError{10};
                    stream->headers.erase(address);
                    memory.store(GuestAddress{address+4},32,flags&~acm::status_prepared);
                }
                else if(name=="acmStreamConvert"){
                    if(d(2)&~0x34U)throw AcmError{10};
                    const auto found=stream->headers.find(address);
                    if(found==stream->headers.end())throw AcmError{514};
                    const auto& expected=found->second;
                    if(src!=expected.source || dst!=expected.destination ||
                       src_size>expected.source_length || dst_size>expected.destination_length)throw AcmError{11};
                    memory.store(GuestAddress{address+source_length+4},32,0);memory.store(GuestAddress{address+destination_length+4},32,0);
                    memory.store(GuestAddress{address+4},32,flags&~acm::status_done);
                    std::vector<std::uint8_t> source(src_size),destination(dst_size);if(src_size)memory.read(GuestAddress{src},source);memory.check(GuestAddress{dst},dst_size,Access::Write);if(d(2)&0x10)stream->reset();
                    const auto [consumed,written]=stream->convert(source,destination,d(2));consumed_bytes=consumed;written_bytes=written;
                    if(written)memory.write(GuestAddress{dst},std::span(destination).first(written));
                    memory.store(GuestAddress{address+source_length+4},32,consumed);memory.store(GuestAddress{address+destination_length+4},32,written);
                    memory.store(GuestAddress{address+4},32,flags|acm::status_prepared|acm::status_done);
                }else throw HostCallFault("未知 ACM 服務");
            }
        }
    }catch(const AcmError& error){result=error.code;}catch(const MemoryFault&){result=11;}catch(const std::bad_alloc&){result=7;}catch(const std::invalid_argument&){result=512;}
    static const bool trace=std::getenv("X64ARM_TRACE_ACM")!=nullptr;
    if(trace){if(!acm_)acm_=std::make_shared<AcmState>();const auto count=++acm_->trace_calls[name];if(count<=16 || (count&(count-1))==0){
        std::fprintf(stderr,"[acm-call] method=%.*s count=%llu result=%u source_tag=%u destination_tag=%u channels=%u->%u rate=%u->%u consumed=%u written=%u\n",
            static_cast<int>(name.size()),name.data(),static_cast<unsigned long long>(count),result,traced?traced->source.tag:0,traced?traced->destination.tag:0,
            traced?traced->source.channels:0,traced?traced->destination.channels:0,traced?traced->source.rate:0,traced?traced->destination.rate:0,consumed_bytes,written_bytes);
    }}
    arg.return_to(state,result);return {};
}
}
