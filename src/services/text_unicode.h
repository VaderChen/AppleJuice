#pragma once
/* IsTextUnicode／RtlIsTextUnicode 共用的位元測試與統計判斷，供宿主及常駐客體常式使用。
 * 這是 Windows 的文字啟發式，不是 UTF-16 合法性驗證；不讀超過呼叫端指定長度。 */
static inline int x64arm_text_unicode(const unsigned char *text,int length,unsigned *tests){
    const unsigned requested=*tests;unsigned found=0;
    if(length<2){*tests=(0x2000U|((length>0 && (length&1))?0x200U:0U))&requested;return 0;}
    if(length&1)found|=0x200U;
    unsigned count=(unsigned)length/2;if(count>256)count=256;
    if(text[0]==0xff && text[1]==0xfe)found|=8U;
    if(text[0]==0xfe && text[1]==0xff)found|=0x80U;
    unsigned low_delta=0,high_delta=0,previous_low=0,previous_high=0,ascii=1,reverse_ascii=1;
    for(unsigned i=0;i<count;++i){
        const unsigned low=text[2*i],high=text[2*i+1],value=low|(high<<8);
        low_delta+=low>previous_low?low-previous_low:previous_low-low;
        high_delta+=high>previous_high?high-previous_high:previous_high-high;
        previous_low=low;previous_high=high;
        if(high || low>127)ascii=0;if(low || high>127)reverse_ascii=0;
        if(!low || !high)found|=0x1000U;
        if(value==9 || value==10 || value==13 || value==32 || value==0x3000)found|=4U;
        if(value==0x900 || value==0xa00 || value==0xd00 || value==0x2000 || value==0x30)found|=0x40U;
        if(value==0xffff || (value==0xfffe && i) || value==0xa0d || value==0xd0a || (!value && i+1<count))found|=0x100U;
    }
    if(ascii && low_delta)found|=1U;if(reverse_ascii && high_delta)found|=0x10U;
    if(low_delta>3*high_delta)found|=2U;if(high_delta>3*low_delta)found|=0x20U;
    found&=requested;*tests=found;
    if(found&(0xf0U|0xf00U))return 0;
    return (found&(0xfU|0x1000U))!=0;
}
