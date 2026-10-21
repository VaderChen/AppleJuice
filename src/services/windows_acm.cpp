#include "x64arm/windows_services.hpp"
#include <algorithm>
#include <cstring>
#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#endif
namespace x64arm {
namespace {
struct AcmError {std::uint32_t code;};
struct Format {unsigned tag{},channels{},rate{},average{},align{},bits{};};
Format read_format(GuestMemory& memory,std::uint64_t pointer){
    if(!pointer)throw AcmError{11};const auto get=[&](unsigned at,std::uint8_t bits){return static_cast<unsigned>(memory.load(GuestAddress{pointer+at},bits));};
    Format f{get(0,16),get(2,16),get(4,32),get(8,32),get(12,16),get(14,16)};
    if(!f.channels || f.channels>2 || f.rate<1000 || f.rate>384000 || !f.align || !f.average)throw AcmError{512};
    if(f.tag==1 || f.tag==3){if((f.tag==3 && f.bits!=32) || (f.bits!=8 && f.bits!=16 && f.bits!=24 && f.bits!=32) || f.align!=f.channels*f.bits/8 || f.average!=f.rate*f.align)throw AcmError{512};}
    else if(f.tag!=0x55)throw AcmError{512};return f;
}
#ifdef __APPLE__
AudioStreamBasicDescription description(Format f){AudioStreamBasicDescription d{};d.mSampleRate=f.rate;d.mChannelsPerFrame=f.channels;
    if(f.tag==0x55){d.mFormatID=kAudioFormatMPEGLayer3;d.mFramesPerPacket=f.rate>=32000?1152:576;}
    else{d.mFormatID=kAudioFormatLinearPCM;d.mFormatFlags=kAudioFormatFlagIsPacked|(f.tag==3?kAudioFormatFlagIsFloat:f.bits>8?kAudioFormatFlagIsSignedInteger:0);d.mBitsPerChannel=f.bits;d.mFramesPerPacket=1;d.mBytesPerFrame=d.mBytesPerPacket=f.align;}return d;}
#endif
struct Stream {
    Format source,destination;std::map<std::uint64_t,std::pair<unsigned,unsigned>> headers;
#ifdef __APPLE__
    AudioConverterRef converter{};std::span<const std::uint8_t> input;std::vector<std::uint8_t> retained;std::size_t cursor{};bool ending{};AudioStreamPacketDescription packet{};
    Stream(Format s,Format d):source(s),destination(d){auto in=description(s),out=description(d);if(d.tag==0x55 || AudioConverterNew(&in,&out,&converter)!=noErr)throw AcmError{512};}
    ~Stream(){if(converter)AudioConverterDispose(converter);}
    void reset(){if(AudioConverterReset(converter)!=noErr)throw AcmError{512};}
    static OSStatus supply(AudioConverterRef,UInt32* count,AudioBufferList* buffers,AudioStreamPacketDescription** packets,void* context){
        auto& self=*static_cast<Stream*>(context);const auto bytes=self.input.subspan(self.cursor);unsigned length=0,frames=0;
        if(self.source.tag==0x55){
            if(bytes.size()>=4){const std::uint32_t header=(std::uint32_t{bytes[0]}<<24)|(std::uint32_t{bytes[1]}<<16)|(std::uint32_t{bytes[2]}<<8)|bytes[3];const auto version=(header>>19)&3,layer=(header>>17)&3,bitrate=(header>>12)&15,frequency=(header>>10)&3;
                if((header&0xffe00000U)!=0xffe00000U || version==1 || layer!=1 || !bitrate || bitrate==15 || frequency==3){*count=0;return kAudioConverterErr_InvalidInputSize;}
                constexpr unsigned rates[]{44100,48000,32000},mpeg1[]{0,32,40,48,56,64,80,96,112,128,160,192,224,256,320},mpeg2[]{0,8,16,24,32,40,48,56,64,80,96,112,128,144,160};const auto rate=rates[frequency]/(version==3?1:version==2?2:4);
                length=(version==3?144000U:72000U)*(version==3?mpeg1[bitrate]:mpeg2[bitrate])/rate+((header>>9)&1);if(length>bytes.size())length=0;frames=version==3?1152:576;
            }
            if(length){*count=1;self.packet={0,frames,length};if(packets)*packets=&self.packet;}
        }else{*count=std::min(*count,static_cast<UInt32>(bytes.size()/self.source.align));length=*count*self.source.align;if(packets)*packets=nullptr;}
        if(!length){*count=0;return self.ending?noErr:1;}
        // Core Audio 可保留本次 callback 的資料，直到下次索取；不可指向呼叫端的短命 staging。
        try{self.retained.assign(bytes.begin(),bytes.begin()+length);}catch(const std::bad_alloc&){*count=0;return -108;}
        buffers->mNumberBuffers=1;buffers->mBuffers[0]={self.source.channels,length,self.retained.data()};self.cursor+=length;return noErr;
    }
    std::pair<unsigned,unsigned> convert(std::span<const std::uint8_t> bytes,std::span<std::uint8_t> output,bool end){input=bytes;cursor=0;ending=end;UInt32 frames=static_cast<UInt32>(output.size()/destination.align);AudioBufferList buffer{1,{{destination.channels,static_cast<UInt32>(output.size()),output.data()}}};
        const auto result=AudioConverterFillComplexBuffer(converter,supply,this,&frames,&buffer,nullptr);const auto consumed=static_cast<unsigned>(cursor);input={};if(result!=noErr && result!=1)throw AcmError{512};return {consumed,frames*destination.align};}
#else
    Stream(Format,Format){throw AcmError{512};}void reset(){throw AcmError{512};}
    std::pair<unsigned,unsigned> convert(std::span<const std::uint8_t>,std::span<std::uint8_t>,bool){throw AcmError{512};}
#endif
};
}
struct AcmState {std::map<std::uint64_t,std::shared_ptr<Stream>> streams;};
std::optional<std::uint32_t> WindowsServices::invoke_acm(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const auto d=[&](unsigned n){return static_cast<unsigned>(arg(n));};const std::uint8_t pointer_bits=x86_?32:64;std::uint32_t result=0;
    try{
        if(name=="acmGetVersion"){arg.return_to(state,0x04000000);return {};}
        if(name=="acmMetrics"){if(!arg(2))throw AcmError{11};unsigned value=0;if(arg(0))throw AcmError{5};switch(d(1)){case 1:case 2:case 3:value=1;break;case 4:case 5:value=0;break;case 50:value=30;break;default:throw AcmError{10};}memory.store(GuestAddress{arg(2)},32,value);}
        else if(name=="acmFormatSuggest"){
            if(arg(0))throw AcmError{5};const auto source=read_format(memory,arg(1));if(!arg(2) || d(3)<16)throw AcmError{11};if(d(4)&~0xf0000U)throw AcmError{10};const auto get=[&](unsigned at,std::uint8_t bits){return static_cast<unsigned>(memory.load(GuestAddress{arg(2)+at},bits));};
            const auto tag=d(4)&0x10000?get(0,16):1U,channels=d(4)&0x20000?get(2,16):source.channels,rate=d(4)&0x40000?get(4,32):source.rate,bits=d(4)&0x80000?get(14,16):16U;
            if(tag!=1 || !channels || channels>2 || rate<1000 || rate>384000 || (bits!=8 && bits!=16 && bits!=24 && bits!=32))throw AcmError{512};const auto align=channels*bits/8;memory.check(GuestAddress{arg(2)},std::min(d(3),18U),Access::Write);
            memory.store(GuestAddress{arg(2)},16,tag);memory.store(GuestAddress{arg(2)+2},16,channels);memory.store(GuestAddress{arg(2)+4},32,rate);memory.store(GuestAddress{arg(2)+8},32,rate*align);memory.store(GuestAddress{arg(2)+12},16,align);memory.store(GuestAddress{arg(2)+14},16,bits);if(d(3)>=18)memory.store(GuestAddress{arg(2)+16},16,0);
        }else if(name=="acmStreamOpen"){
            if(d(7)&~7U)throw AcmError{10};if(arg(1))throw AcmError{5};if(arg(4) || (d(7)&2))throw AcmError{512};if(arg(5) || arg(6) || (!(d(7)&1) && !arg(0)))throw AcmError{11};if(arg(0))memory.store(GuestAddress{arg(0)},pointer_bits,0);
            auto stream=std::make_shared<Stream>(read_format(memory,arg(2)),read_format(memory,arg(3)));if(!(d(7)&1)){if(!acm_)acm_=std::make_shared<AcmState>();if(acm_->streams.size()>=256)throw AcmError{4};const auto handle=runtime_.allocate_wait_handle();acm_->streams.emplace(handle,std::move(stream));memory.store(GuestAddress{arg(0)},pointer_bits,handle);}
        }else{
            if(name=="acmStreamSize" && arg(2))memory.store(GuestAddress{arg(2)},32,0);if(!acm_ || !acm_->streams.contains(arg(0)))throw AcmError{5};auto stream=acm_->streams.at(arg(0));
            if(name=="acmStreamSize"){
                if(!arg(2) || !d(1))throw AcmError{11};if(d(3)>1)throw AcmError{10};const auto& in=d(3)?stream->destination:stream->source;const auto& out=d(3)?stream->source:stream->destination;
                auto size=(std::uint64_t{d(1)}*out.average+in.average-1)/in.average;if(in.tag==0x55)size+=out.align*2304U;size=(size+out.align-1)/out.align*out.align;if(size>UINT32_MAX)throw AcmError{512};memory.store(GuestAddress{arg(2)},32,std::max<std::uint64_t>(out.align,size));
            }else if(name=="acmStreamClose"){if(d(1))throw AcmError{10};if(!stream->headers.empty())throw AcmError{513};acm_->streams.erase(arg(0));}
            else if(name=="acmStreamReset"){if(d(1))throw AcmError{10};stream->reset();}
            else if(name=="acmStreamMessage")throw AcmError{8};
            else{
                const auto address=arg(1);if(!address)throw AcmError{11};const auto size=x86_?84U:104U,source_pointer=x86_?12U:16U,source_length=x86_?16U:24U,destination_pointer=x86_?28U:40U,destination_length=x86_?32U:48U;
                if(memory.load(GuestAddress{address},32)<size)throw AcmError{11};memory.check(GuestAddress{address},size,Access::Write);const auto flags=static_cast<unsigned>(memory.load(GuestAddress{address+4},32));
                const auto src=memory.load(GuestAddress{address+source_pointer},pointer_bits),dst=memory.load(GuestAddress{address+destination_pointer},pointer_bits);const auto src_size=static_cast<unsigned>(memory.load(GuestAddress{address+source_length},32)),dst_size=static_cast<unsigned>(memory.load(GuestAddress{address+destination_length},32));
                if(name=="acmStreamPrepareHeader"){if(d(2))throw AcmError{10};if(src_size>64U*1024U*1024U || dst_size>64U*1024U*1024U || (!src && src_size) || !dst || !dst_size)throw AcmError{11};memory.check(GuestAddress{src},src_size,Access::Read);memory.check(GuestAddress{dst},dst_size,Access::Write);stream->headers[address]={src_size,dst_size};memory.store(GuestAddress{address+4},32,(flags&~1U)|0x20000);}
                else if(name=="acmStreamUnprepareHeader"){if(d(2))throw AcmError{10};stream->headers.erase(address);memory.store(GuestAddress{address+4},32,flags&~0x20000U);}
                else if(name=="acmStreamConvert"){
                    if(d(2)&~0x34U)throw AcmError{10};const auto found=stream->headers.find(address);if(found==stream->headers.end())throw AcmError{514};if(src_size>found->second.first || dst_size>found->second.second)throw AcmError{11};
                    if(d(2)&0x10)stream->reset();std::vector<std::uint8_t> source(src_size),destination(dst_size);if(src_size)memory.read(GuestAddress{src},source);memory.check(GuestAddress{dst},dst_size,Access::Write);
                    const auto [consumed,written]=stream->convert(source,destination,(d(2)&0x20)!=0);if(written)memory.write(GuestAddress{dst},std::span(destination).first(written));memory.store(GuestAddress{address+source_length+4},32,consumed);memory.store(GuestAddress{address+destination_length+4},32,written);memory.store(GuestAddress{address+4},32,flags|0x20001);
                }else throw HostCallFault("未知 ACM 服務");
            }
        }
    }catch(const AcmError& error){result=error.code;}catch(const MemoryFault&){result=11;}catch(const std::bad_alloc&){result=7;}
    arg.return_to(state,result);return {};
}
}
