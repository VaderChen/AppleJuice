/* 小輸出使用同一個有界暫存，不為格式家族各複製一份 parser。 */
__attribute__((noinline)) static Word FORMAT_BUFFER_NAME(void* destination,Size capacity,Size limit,
        const void* specification,const void* arguments,Word options,unsigned mode) {
    if(!specification || ((mode&X64ARM_FMT_COMMON) && (options&~0x37ULL)) ||
       ((mode&X64ARM_FMT_LIMITED) && !limit))return ~0ULL;
    if(!(mode&X64ARM_FMT_COUNT) && (((mode&X64ARM_FMT_SECURE) && (!destination || !capacity)) ||
       (!destination && capacity)))return ~0ULL;
    FORMAT_OUTPUT text[512];Size count=0,at=0;
    const int wide=FORMAT_WIDE;
    const int wide_strings=wide && (!(mode&X64ARM_FMT_COMMON) || (options&4));
    const Byte* args=(const Byte*)arguments;
    for(;;) {
        unsigned c=format_unit(specification,at++,wide);
        if(!c)break;
        if(c!='%') {if(count==512)return ~0ULL;text[count++]=(FORMAT_OUTPUT)c;continue;}
        c=format_unit(specification,at++,wide);
        if(c=='%') {if(count==512)return ~0ULL;text[count++]='%';continue;}
        unsigned flags=0,flag;
        while((flag=x64arm_crt_format_flag(c))) {flags|=flag;c=format_unit(specification,at++,wide);}
        int width=0,precision=-1;
        if(c=='*') {width=(int)format_argument(&args,4);c=format_unit(specification,at++,wide);}
        else while(c>='0' && c<='9') {if(width>6553)return ~0ULL;width=width*10+(int)c-'0';c=format_unit(specification,at++,wide);}
        if(c=='.') {
            c=format_unit(specification,at++,wide);precision=0;
            if(c=='*') {precision=(int)format_argument(&args,4);c=format_unit(specification,at++,wide);}
            else while(c>='0' && c<='9') {if(precision>6553)return ~0ULL;precision=precision*10+(int)c-'0';c=format_unit(specification,at++,wide);}
        }
        if(width<-65536 || width>65536 || precision>65536)return ~0ULL;
        if(width<0) {flags|=X64ARM_FMT_LEFT;width=-width;}
        unsigned length=0,bits=32;
        if(c=='h' || c=='l') {
            length=c;c=format_unit(specification,at++,wide);
            if(c==length) {bits=length=='h'?8:64;length|=0x100;c=format_unit(specification,at++,wide);}
            else if(length=='h')bits=16;
        } else if(c=='I') {
            length=c;bits=64;c=format_unit(specification,at++,wide);
            if(c=='3' || c=='6') {
                unsigned next=format_unit(specification,at++,wide);
                if((c=='3' && next!='2') || (c=='6' && next!='4'))return ~0ULL;
                bits=c=='3'?32:64;length|=0x100;c=format_unit(specification,at++,wide);
            }
        } else if(c=='z' || c=='t' || c=='j' || c=='L' || c=='w' || c=='T') {
            length=c;if(c=='z' || c=='t' || c=='j')bits=64;c=format_unit(specification,at++,wide);
        }
        if(c=='d' || c=='i' || c=='u' || c=='o' || c=='x' || c=='X' || c=='p') {
            if(length=='T' || (c!='p' && (length=='L' || length=='w')))return ~0ULL;
            const Word value=format_argument(&args,c=='p' || bits==64?8:4);
            X64armCrtInteger number=x64arm_crt_integer(value,bits,c,flags,(unsigned)width,precision,8);
            const unsigned total=number.spaces+number.prefix_count+number.zeros+number.count+number.trailing;
            if(total>512-count)return ~0ULL;
            for(unsigned n=0;n<number.spaces;++n)text[count++]=' ';
            for(unsigned n=0;n<number.prefix_count;++n)text[count++]=(FORMAT_OUTPUT)number.prefix[n];
            for(unsigned n=0;n<number.zeros;++n)text[count++]='0';
            while(number.count)text[count++]=(FORMAT_OUTPUT)number.digits[--number.count];
            for(unsigned n=0;n<number.trailing;++n)text[count++]=' ';
        } else if(c=='s' || c=='S' || c=='c' || c=='C') {
            if(length && length!='h' && length!='l' && length!='w' && length!='T')return ~0ULL;
            const int string=c=='s' || c=='S';
            const int source_wide=length=='T'?wide:length=='l' || length=='w' ||
                (!length && ((c=='S' || c=='C')?!wide_strings:wide_strings));
            const Word value=format_argument(&args,string?8:4);
            Wide single=(Wide)(value&(source_wide?65535:255));
            const void* source=(const void*)(Address)value;
            int source_units=source_wide;Size size=0;
            if(!string) {if(source_wide!=wide)return ~0ULL;source=&single;source_units=1;size=1;}
            else if(!value) {source="(null)";source_units=0;size=6;if(precision>=0 && (unsigned)precision<size)size=(unsigned)precision;}
            else {
                if(source_wide!=wide)return ~0ULL;
                const Size maximum=precision<0?513:(unsigned)precision;
                while(size<maximum && format_unit(source,size,source_units)) {if(++size>512)return ~0ULL;}
            }
            const Size field=size>(unsigned)width?size:(unsigned)width;
            if(field>512-count)return ~0ULL;
            if(!(flags&X64ARM_FMT_LEFT))for(Size n=size;n<field;++n)text[count++]=' ';
            for(Size n=0;n<size;++n)text[count++]=(FORMAT_OUTPUT)format_unit(source,n,source_units);
            if(flags&X64ARM_FMT_LEFT)for(Size n=size;n<field;++n)text[count++]=' ';
        } else return ~0ULL;
    }
    if((mode&X64ARM_FMT_COUNT) || (!destination && !capacity)) {return count;}
    const X64armCrtBufferPlan plan=x64arm_crt_buffer_plan(count,0,capacity,limit,~0ULL,options,mode);
    if(plan.invalid_parameter || plan.errno_after_write)return ~0ULL;
    if(wide) {
        for(Size n=0;n<plan.length;++n)((Wide*)destination)[n]=text[n];
        if(plan.terminate)((Wide*)destination)[plan.length]=0;
    } else {
        for(Size n=0;n<plan.length;++n)((Byte*)destination)[n]=(Byte)text[n];
        if(plan.terminate)((Byte*)destination)[plan.length]=0;
    }
    return plan.result;
}
