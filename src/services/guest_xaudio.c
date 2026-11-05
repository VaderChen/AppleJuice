#include "xaudio_layout.h"
typedef __UINT32_TYPE__ Word;
#if defined(__i386__)
#define CALL __attribute__((stdcall))
#else
#define CALL
#endif
#define EXPORT __declspec(dllexport) Word CALL
#define CELL(self,offset) (*(Word*)((unsigned char*)(self)+(offset)))
struct __attribute__((packed,may_alias)) Value {Word bits;};
static void copy_words(void* out,const void* source,unsigned count){
    struct Value* to=out;const struct Value* from=source;
    /* 輸出先探測完整範圍；故障不留下部分寫入。 */
    for(unsigned i=0;i<count;++i){volatile struct Value* probe=to+i;Word old=probe->bits;probe->bits=old;}
    for(unsigned i=0;i<count;++i)to[i].bits=from[i].bits;
}
EXPORT GetVoiceDetails(void* self,void* out){
    Word values[4]={CELL(self,XA_FLAGS),CELL(self,XA_ACTIVE),CELL(self,XA_CHANNELS),CELL(self,XA_RATE)};
    if(CELL(self,XA_VERSION)==7){values[1]=values[2];values[2]=values[3];copy_words(out,values,3);}else copy_words(out,values,4);return 0;
}
EXPORT GetVolume(void* self,void* out){copy_words(out,(unsigned char*)self+XA_VOLUME,1);return 0;}
EXPORT GetFrequencyRatio(void* self,void* out){copy_words(out,(unsigned char*)self+XA_RATIO,1);return 0;}
EXPORT GetFilterParameters(void* self,void* out){if(!(CELL(self,XA_FLAGS)&8U))return 0x88960001U;copy_words(out,(unsigned char*)self+XA_FILTER,3);return 0;}
EXPORT GetChannelVolumes(void* self,Word channels,void* out){if(channels!=CELL(self,XA_CHANNELS))return 0x88960001U;copy_words(out,(unsigned char*)self+XA_VOLUMES,channels);return 0;}
EXPORT GetChannelMask(void* self,void* out){if(!out)return 0x80004003U;copy_words(out,(unsigned char*)self+XA_MASK,1);return 0;}
static Word voice_state(void* self,void* out){
    Word values[sizeof(void*)/4+3];
    values[0]=CELL(self,XA_CONTEXT);
#if !defined(__i386__)
    values[1]=CELL(self,XA_CONTEXT+4);
#endif
    values[sizeof(void*)/4]=CELL(self,XA_QUEUED);
    values[sizeof(void*)/4+1]=CELL(self,XA_SAMPLES);
    values[sizeof(void*)/4+2]=CELL(self,XA_SAMPLES+4);
    copy_words(out,values,sizeof values/sizeof values[0]);return 0;
}
EXPORT GetState27(void* self,void* out){return voice_state(self,out);}
EXPORT GetState28(void* self,void* out,Word flags){if(flags&~256U)return 0x88960001U;return voice_state(self,out);}
