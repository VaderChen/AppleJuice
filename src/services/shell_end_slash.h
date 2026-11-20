#pragma once
#include "shell_root_query.h"

/* 傳統入口直接回傳路徑內的指標。A 版的 CharPrev 依客體 ACP（UTF-8）回到碼點開頭。 */
static inline ShellRootAddress shell_legacy_end_slash(struct ShellRootInput p,int add,ShellRootWrite write)
{
    if(!p.path)return 0;
    ShellRootAddress n=0,last=0;
    unsigned c;
    while((c=shell_root_char(p,n))){
        if(p.unit==2 || (c&0xc0)!=0x80)last=n;
        ++n;
        if(add && n>=260)return 0;
    }
    if(!n)return p.path;
    const int slash=shell_root_char(p,last)=='\\';
    if(add){
        if(!slash){
            write(p.context,p.path+n*p.unit,p.unit,'\\');
            ++n;
            write(p.context,p.path+n*p.unit,p.unit,0);
        }
        return p.path+n*p.unit;
    }
    if(slash && !shell_root_query(p,ShellIsRoot,0,write))write(p.context,p.path+last*p.unit,p.unit,0);
    return p.path+last*p.unit;
}

/* end 與 remaining 在需要寫回輸出時才更新，保留未終止輸入的錯誤契約。 */
static inline unsigned shell_cch_end_slash(struct ShellRootInput p,ShellRootAddress size,int add,
    ShellRootAddress *end,ShellRootAddress *remaining,ShellRootWrite write)
{
    if(!add && (!size || !end || !remaining)){
        if(end)*end=0;
        if(remaining)*remaining=0;
        return SHELL_ROOT_INVALID;
    }
    ShellRootAddress n=0;
    if(p.path)while(n<size && shell_root_char(p,n))++n;
    int slash=n && shell_root_char(p,n-1)=='\\';
    if(add && (n==size || (n && !slash && n+1==size))){
        if(end)*end=0;
        if(remaining)*remaining=0;
        return 0x8007007aU;
    }
    if(!add && (!p.path || n==size))return SHELL_ROOT_INVALID;
    unsigned status=1;
    if(add && n && !slash){
        write(p.context,p.path+n*2,2,'\\');
        write(p.context,p.path+(++n)*2,2,0);
        status=0;
    }else if(!add && slash){
        --n;
        if(!shell_root_query(p,ShellIsRoot,0,write)){
            write(p.context,p.path+n*2,2,0);
            status=0;
        }
    }
    if(end)*end=p.path+n*2;
    if(remaining)*remaining=size-n;
    return status;
}
