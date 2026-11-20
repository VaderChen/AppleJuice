#pragma once
#include "shell_root_query.h"

/* 舊版組合介面先複製來源，再於暫存區整理句點，最後才寫入可重疊的目的地。
 * 專案 ACP 是 UTF-8。暫存區容納 259 個 UTF-16 碼元的最長 UTF-8 編碼。 */
static inline unsigned shell_join_width(unsigned unit,unsigned ch)
{
    return unit==2 ? 1 : (ch&0xc0)==0x80 ? 0 : ch>=0xf0 ? 2 : 1;
}

static inline unsigned shell_join_read_local(void *ctx,ShellRootAddress at,unsigned unit)
{
    (void)unit;
    return ((unsigned short *)ctx)[(at-1)/2];
}
static inline void shell_join_write_local(void *ctx,ShellRootAddress at,unsigned unit,unsigned ch)
{
    (void)unit;
    ((unsigned short *)ctx)[(at-1)/2]=(unsigned short)ch;
}

static inline unsigned shell_join_normalize(const unsigned short *path,unsigned length,unsigned short *out)
{
    unsigned src=0,dst=0;
    if(!length){out[0]='\\';return 1;}
    if(path[0]=='\\')out[dst++]=path[src++];
    else if(length>=2 && path[1]==':'){
        out[dst++]=path[src++];out[dst++]=path[src++];
        if(path[src]=='\\')out[dst++]=path[src++];
    }
    while(src<length){
        if(path[src]=='.' && path[src+1]=='\\' &&
           (!src || path[src-1]=='\\' || path[src-1]==':')){
            src+=2;
        }else if(path[src]=='.' && path[src+1]=='.' && dst && out[dst-1]=='\\'){
            unsigned slashes=0;
            if(dst>=2 && out[0]=='\\' && out[1]=='\\')
                for(unsigned i=2;i<dst;++i)slashes+=out[i]=='\\';
            out[dst]=0;
            if(dst>1 && (out[dst-2]!='\\' || dst>2)){
                if(out[dst-2]==':' && (dst>3 || (dst>=3 && out[dst-3]==':'))){
                    dst-=2;
                    while(dst && out[dst]!='\\')--dst;
                    if(out[dst]=='\\')++dst;
                }else if(out[dst-2]!=':' && slashes!=1)dst-=2;
            }
            while(dst && out[dst]!='\\')--dst;
            src+=2;
            if(!dst){out[dst++]='\\';if(src<length)++src;}
        }else out[dst++]=path[src++];
    }
    if(dst==2 && out[1]==':')out[dst++]='\\';
    return dst;
}

static inline ShellRootAddress shell_path_join(struct ShellRootInput out,ShellRootAddress dir,
    ShellRootAddress file,int append,ShellRootWrite write)
{
    if(!out.path || (append && !file))return 0;
    unsigned short tmp[781];
    unsigned length=0,units=0;
    if(dir || file){
        struct ShellRootInput a=out,b=out;
        a.path=dir;b.path=file;
        unsigned first=file?shell_root_char(b,0):0;
        int unc=first=='\\' && shell_root_char(b,1)=='\\';
        if(append && first=='\\' && !unc){b.path+=b.unit;first=shell_root_char(b,0);}
        int absolute=first && (unc || shell_root_char(b,1)==':');
        int both=dir && shell_root_char(a,0) && first && !absolute;
        struct ShellRootInput input=(!first && dir) || both?a:b;
        if(out.unit==1){
            struct ShellRootInput sources[2]={a,b};
            for(unsigned i=0;i<2;++i){
                unsigned count=0;
                for(unsigned j=0;sources[i].path;++j){
                    unsigned ch=shell_root_char(sources[i],j);
                    if(!ch)break;
                    count+=shell_join_width(1,ch);
                    if(count>=260 || j>=777)goto fail;
                }
            }
        }
        for(;input.path && length<777;++length){
            unsigned ch=shell_root_char(input,length);
            if(!ch || units+shell_join_width(out.unit,ch)>=260)break;
            units+=shell_join_width(out.unit,ch);tmp[length]=(unsigned short)ch;
        }
        tmp[length]=0;
        if(both){
            if(first=='\\'){
                /* 本機暫存索引從 1 起算，以保留 0 作為 NULL。 */
                struct ShellRootInput local={tmp,1,2,shell_join_read_local};
                shell_root_query(local,ShellStripRoot,778,shell_join_write_local);
                length=units=0;
                while(tmp[length])units+=shell_join_width(out.unit,tmp[length++]);
                b.path+=b.unit;
            }
            if(length && tmp[length-1]!='\\'){
                if(++units>=260)goto fail;
                tmp[length++]='\\';
            }
            for(unsigned i=0;;++i){
                unsigned ch=shell_root_char(b,i);
                if(!ch)break;
                units+=shell_join_width(out.unit,ch);
                if(units>=260 || length>=777)goto fail;
                tmp[length++]=(unsigned short)ch;
            }
            tmp[length]=0;
        }
        unsigned short normalized[781];
        /* 沿用 Shell 對起始相對目錄的處理，保留第一個反斜線。 */
        unsigned skip=0;
        if(tmp[0]=='.'){
            if(tmp[1]=='\\')skip=1;
            else if(tmp[1]=='.' && tmp[2]=='\\')skip=2;
        }
        length=shell_join_normalize(tmp+skip,length-skip,normalized);
        if(out.unit==2 || length<260){
            for(unsigned i=0;i<=length;++i)
                write(out.context,out.path+i*out.unit,out.unit,i<length?normalized[i]:0);
            return append?1:out.path;
        }
    }
fail:
    write(out.context,out.path,out.unit,0);
    return 0;
}
