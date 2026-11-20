#pragma once
#include "shell_root_query.h"

/* 舊 Shell 保護磁碟根，但允許移除 UNC 的共用名稱。PathCch 則保留共用名稱。 */
static inline unsigned shell_remove_file_spec(struct ShellRootInput p,int cch,
    ShellRootAddress capacity,ShellRootWrite write)
{
    if(!p.path || (cch && (!capacity || capacity>32768)))return cch?SHELL_ROOT_INVALID:0;
    if(!shell_root_char(p,0))return cch?1:0;
    ShellRootAddress floor=0;
    const int extended=shell_root_prefix(p,0,"\\\\?\\");
    if(cch || (extended && p.unit==2 && !shell_root_prefix(p,4,"UNC\\"))){
        ShellRootAddress after=shell_root_query(p,ShellSkipCchRoot,0,write);
        if(after && after!=SHELL_ROOT_KEEP_OUTPUT)floor=(after-p.path)/2;
        if(cch && floor && shell_root_char(p,floor-1)=='\\' &&
           (extended ? shell_root_prefix(p,4,"UNC\\") && shell_root_char(p,8) :
                       shell_root_prefix(p,0,"\\\\") && shell_root_char(p,2)))
            --floor;
    }
    if(!cch && !floor){
        if(shell_root_drive(p,0))floor=2+(shell_root_char(p,2)=='\\');
        else{
            floor=shell_root_char(p,0)=='\\';
            if(shell_root_char(p,floor) && shell_root_char(p,floor+1)!='?'){
                if(shell_root_char(p,floor)=='\\')++floor;
                if(floor>1 && shell_root_drive(p,(unsigned)floor))floor+=2;
                if(shell_root_char(p,floor)=='\\' && shell_root_char(p,floor+1) && shell_root_char(p,floor+1)!='\\')++floor;
            }
        }
    }
    ShellRootAddress cut=floor;
    for(ShellRootAddress i=floor;;++i){
        unsigned ch=shell_root_char(p,i);
        if(!ch)break;
        if(ch=='\\')cut=i;
    }
    if(cut>floor && shell_root_char(p,cut-1)=='\\' && (!cch || shell_root_char(p,cut+1)!='?'))--cut;
    if(cch && cut>=capacity)return SHELL_ROOT_INVALID;
    if(!shell_root_char(p,cut))return cch?1:0;
    write(p.context,p.path+cut*p.unit,p.unit,0);
    return cch?0:1;
}
