#pragma once
/* 無平台狀態的影像配置計算，供客體 resident AOT 與宿主回退共用。 */
typedef __UINT32_TYPE__ MfImageWord;
typedef __UINT64_TYPE__ MfImageWide;
typedef __INT32_TYPE__ MfImageLong;
typedef __UINT8_TYPE__ MfImageByte;
#define MF_IMAGE_FOURCC(a,b,c,d) ((MfImageWord)(a)|((MfImageWord)(b)<<8)|((MfImageWord)(c)<<16)|((MfImageWord)(d)<<24))
#define MF_IMAGE_INVALID 0x80070057U
#define MF_IMAGE_POINTER 0x80004003U
#define MF_IMAGE_TYPE 0xc00d36b4U
#define MF_IMAGE_OVERFLOW 0x80070216U
typedef struct {MfImageWide stride;unsigned bits,alignment,scale,divisor;int rgb;} MfImageLayout;

static inline MfImageLayout mf_image_layout(MfImageWord format,MfImageWord width){
    MfImageLayout out={0,0,1,1,1,0};
    switch(format){
    case 118:out.bits=1;out.rgb=1;break;
    case MF_IMAGE_FOURCC('4','P','x','x'):out.bits=4;out.rgb=1;break;
    case 41:out.bits=8;out.rgb=1;break;
    case 23:case 24:case 25:case 26:out.bits=16;out.rgb=1;break;
    case 20:out.bits=24;out.rgb=1;break;
    case 21:case 22:case 31:case 32:case 33:case 35:out.bits=32;out.rgb=1;break;
    case 36:case 113:out.bits=64;out.rgb=1;break;
    case MF_IMAGE_FOURCC('A','Y','U','V'):case MF_IMAGE_FOURCC('Y','4','1','0'):
    case MF_IMAGE_FOURCC('v','4','1','0'):out.bits=32;break;
    case MF_IMAGE_FOURCC('Y','4','1','6'):out.bits=64;break;
    case MF_IMAGE_FOURCC('A','I','4','4'):case MF_IMAGE_FOURCC('I','A','4','4'):
    case MF_IMAGE_FOURCC('P','8',' ',' '):case MF_IMAGE_FOURCC('L','8',' ',' '):out.bits=8;break;
    case MF_IMAGE_FOURCC('L','1','6',' '):case MF_IMAGE_FOURCC('D','1','6',' '):out.bits=16;break;
    case MF_IMAGE_FOURCC('Y','U','Y','2'):case MF_IMAGE_FOURCC('U','Y','V','Y'):
    case MF_IMAGE_FOURCC('Y','V','Y','U'):case MF_IMAGE_FOURCC('Y','4','2','T'):
        out.bits=16;out.alignment=2;break;
    case MF_IMAGE_FOURCC('Y','2','1','0'):case MF_IMAGE_FOURCC('Y','2','1','6'):
    case MF_IMAGE_FOURCC('v','2','1','6'):out.bits=32;out.alignment=2;break;
    case MF_IMAGE_FOURCC('N','V','1','2'):case MF_IMAGE_FOURCC('Y','V','1','2'):
    case MF_IMAGE_FOURCC('I','4','2','0'):case MF_IMAGE_FOURCC('I','Y','U','V'):
    case MF_IMAGE_FOURCC('I','M','C','2'):case MF_IMAGE_FOURCC('I','M','C','4'):
        out.bits=8;out.alignment=2;out.scale=3;out.divisor=2;break;
    case MF_IMAGE_FOURCC('N','V','1','1'):
        out.bits=8;out.alignment=4;out.scale=3;out.divisor=2;break;
    case MF_IMAGE_FOURCC('P','0','1','0'):case MF_IMAGE_FOURCC('P','0','1','6'):
        out.bits=16;out.alignment=2;out.scale=3;out.divisor=2;break;
    case MF_IMAGE_FOURCC('P','2','1','0'):case MF_IMAGE_FOURCC('P','2','1','6'):
        out.bits=16;out.alignment=2;out.scale=2;break;
    case MF_IMAGE_FOURCC('I','M','C','1'):case MF_IMAGE_FOURCC('I','M','C','3'):
        out.bits=16;out.alignment=2;break;
    case MF_IMAGE_FOURCC('v','2','1','0'):
        out.stride=(MfImageWide)(width/48+(width%48!=0))*128;return out;
    case MF_IMAGE_FOURCC('Y','4','1','P'):case MF_IMAGE_FOURCC('Y','4','1','T'):
        out.stride=((MfImageWide)width+7)/8*12;return out;
    default:return out;
    }
    out.stride=((MfImageWide)width*out.bits+7)/8;
    if(out.rgb)out.stride=(out.stride+3)&~(MfImageWide)3;
    else if(format==MF_IMAGE_FOURCC('I','M','C','1') || format==MF_IMAGE_FOURCC('I','M','C','3'))out.stride=(out.stride+3)&~(MfImageWide)3;
    return out;
}
static inline MfImageWord mf_image_stride(MfImageWord format,MfImageWord width,MfImageLong* result){
    const MfImageLayout d=mf_image_layout(format,width);
    if(!d.bits && !d.stride && format!=MF_IMAGE_FOURCC('v','2','1','0') && format!=MF_IMAGE_FOURCC('Y','4','1','P') && format!=MF_IMAGE_FOURCC('Y','4','1','T'))return MF_IMAGE_TYPE;
    if(d.stride>(d.rgb?0x80000000ULL:0x7fffffffULL))return MF_IMAGE_OVERFLOW;
    *result=(MfImageLong)(d.rgb?0U-(MfImageWord)d.stride:(MfImageWord)d.stride);return 0;
}
static inline MfImageWord mf_image_size(MfImageWord format,MfImageWord width,MfImageWord height,int bitmap,MfImageWord* result){
    MfImageLayout d=mf_image_layout(format,width);*result=0;
    if(!d.bits && !d.stride && format!=MF_IMAGE_FOURCC('v','2','1','0') && format!=MF_IMAGE_FOURCC('Y','4','1','P') && format!=MF_IMAGE_FOURCC('Y','4','1','T'))return bitmap?MF_IMAGE_INVALID:0;
    if(bitmap && !height)return MF_IMAGE_INVALID;
    if(bitmap && !d.rgb && d.bits){
        const MfImageWide aligned=((MfImageWide)width+d.alignment-1)&~(MfImageWide)(d.alignment-1);
        d.stride=(aligned*d.bits+7)/8;
    }
    /* 先驗證可公開的 UINT32 長度，避免乘法在極端尺寸時溢位。 */
    if(d.stride>0xffffffffULL && height)return MF_IMAGE_OVERFLOW;
    const MfImageWide rows=d.stride*height;
    const MfImageWide limit=d.scale==3?0xaaaaaaaaULL:d.scale==2?0x7fffffffULL:0xffffffffULL;
    if(rows>limit)return MF_IMAGE_OVERFLOW;
    *result=(MfImageWord)((rows*d.scale)>>(d.divisor==2));return 0;
}
static inline MfImageWord mf_image_guid_format(const MfImageByte* id){
    const MfImageWord first=(MfImageWord)id[0]|((MfImageWord)id[1]<<8)|((MfImageWord)id[2]<<16)|((MfImageWord)id[3]<<24);
    static const MfImageByte media_tail[12]={0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
    static const MfImageByte rgb_tail[12]={0x4f,0x52,0xce,0x11,0x9f,0x53,0,0x20,0xaf,0x0b,0xa7,0x70};
    unsigned media=1,rgb=1;for(unsigned i=0;i<12;++i){media&=id[i+4]==media_tail[i];rgb&=id[i+4]==rgb_tail[i];}
    if(media)return first;
    if(rgb && first>=0xe436eb78U && first<=0xe436eb7eU){
        static const MfImageWord formats[7]={118,MF_IMAGE_FOURCC('4','P','x','x'),41,23,24,20,22};
        return formats[first-0xe436eb78U];
    }
    return 0;
}
