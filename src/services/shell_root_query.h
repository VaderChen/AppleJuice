#pragma once

typedef unsigned long long ShellRootAddress;
typedef unsigned (*ShellRootRead)(void *,ShellRootAddress,unsigned);
typedef void (*ShellRootWrite)(void *,ShellRootAddress,unsigned,unsigned);
enum ShellRootAction { ShellIsRoot, ShellSkipLegacyRoot, ShellSkipCchRoot, ShellStripRoot };

struct ShellRootInput {
    void *context;
    ShellRootAddress path;
    unsigned unit;
    ShellRootRead read;
};

static inline unsigned shell_root_char(struct ShellRootInput p,ShellRootAddress i)
{
    return p.read(p.context,p.path+i*p.unit,p.unit);
}

static inline int shell_root_prefix(struct ShellRootInput p,unsigned at,const char *prefix)
{
    for (; *prefix; ++at,++prefix) {
        unsigned c=shell_root_char(p,at);
        if(c>='a' && c<='z')c-='a'-'A';
        if(c!=(unsigned char)*prefix)return 0;
    }
    return 1;
}

static inline int shell_root_drive(struct ShellRootInput p,unsigned at)
{
    unsigned c=shell_root_char(p,at);
    return ((c>='A' && c<='Z') || (c>='a' && c<='z')) && shell_root_char(p,at+1)==':';
}

static inline ShellRootAddress shell_root_segment(struct ShellRootInput p,ShellRootAddress at)
{
    unsigned c;
    while ((c=shell_root_char(p,at)) && c!='\\')++at;
    return at;
}

/* cch 的略過回傳客體位址；無效前綴保留輸出，普通相對路徑則清除輸出。 */
#define SHELL_ROOT_KEEP_OUTPUT (~(ShellRootAddress)0)
#define SHELL_ROOT_INVALID 0x80070057U
static inline ShellRootAddress shell_root_query(struct ShellRootInput p,enum ShellRootAction action,
    ShellRootAddress capacity,ShellRootWrite write_unit)
{
    unsigned first=p.path?shell_root_char(p,0):0;
    if(!first || (action==ShellStripRoot && (!capacity || capacity>32768)))
        return action==ShellSkipCchRoot?SHELL_ROOT_KEEP_OUTPUT:action==ShellStripRoot?SHELL_ROOT_INVALID:0;
    int network=first=='\\' && shell_root_char(p,1)=='\\';
    if(action==ShellSkipLegacyRoot){
        if(!network)return shell_root_char(p,1)==':' && shell_root_char(p,2)=='\\'?p.path+3*p.unit:0;
        ShellRootAddress at=shell_root_segment(p,2);
        if(!shell_root_char(p,at))return 0;
        at=shell_root_segment(p,at+1);
        return shell_root_char(p,at)?p.path+(at+1)*p.unit:0;
    }
    if(p.unit==1){
        /* ACP 為 UTF-8；參考實作 的 A 版先在 MAX_PATH 個 UTF-16 碼元內轉換。 */
        unsigned count=0;
        for(ShellRootAddress i=0;;++i){
            unsigned c=shell_root_char(p,i);
            if(!c)break;
            if((c&0xc0)!=0x80)count+=c>=0xf0?2:1;
            if(count>=260)return action==ShellStripRoot?SHELL_ROOT_INVALID:0;
        }
    }
    ShellRootAddress end=0;
    int extended=network && shell_root_char(p,2)=='?';
    if(extended){
        if(shell_root_prefix(p,3,"\\UNC\\"))end=8;
        else if(shell_root_char(p,3)=='\\' && shell_root_drive(p,4)){
            network=0;end=6+(shell_root_char(p,6)=='\\');
        }else if(shell_root_prefix(p,3,"\\VOLUME{")){
            network=0;
            unsigned i;
            for(i=11;i<47;++i){
                unsigned c=shell_root_char(p,i);
                if(i==19 || i==24 || i==29 || i==34){if(c!='-')break;}
                else if(!((c>='0' && c<='9') || (c>='a' && c<='f') || (c>='A' && c<='F')))break;
            }
            if(i==47 && shell_root_char(p,47)=='}')end=48+(shell_root_char(p,48)=='\\');
        }
        if(!end && action==ShellSkipCchRoot)return SHELL_ROOT_KEEP_OUTPUT;
        if(p.unit==1)end=0;
    }else if(network)end=2;
    else if(first=='\\')end=1;
    else if(shell_root_drive(p,0))end=2+(shell_root_char(p,2)=='\\');

    if(network && end){
        ShellRootAddress start=end;
        int has_server=shell_root_char(p,start)!=0;
        end=shell_root_segment(p,start);
        if(shell_root_char(p,end)){
            ++end;
            if(action==ShellIsRoot && !shell_root_char(p,end))return 0;
            if(action==ShellIsRoot || extended || shell_root_char(p,end)!='\\'){
                end=shell_root_segment(p,end);
                if(shell_root_char(p,end)){
                    if(action==ShellIsRoot)return 0;
                    ++end;
                }
            }
        }
        if(action==ShellIsRoot)return 1;
        if(action==ShellStripRoot && has_server && shell_root_char(p,end-1)=='\\')--end;
    }
    if(action==ShellIsRoot)return end && shell_root_char(p,end-1)=='\\' && !shell_root_char(p,end);
    if(action==ShellSkipCchRoot)return end?p.path+end*p.unit:0;
    if(!end){write_unit(p.context,p.path,p.unit,0);return SHELL_ROOT_INVALID;}
    if(p.unit==1 && end>=260){write_unit(p.context,p.path,p.unit,0);return SHELL_ROOT_INVALID;}
    if(end>=capacity)return SHELL_ROOT_INVALID;
    if(!shell_root_char(p,end))return 1; /* S_FALSE */
    write_unit(p.context,p.path+end*p.unit,p.unit,0);
    return 0;
}
