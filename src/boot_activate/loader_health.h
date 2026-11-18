/* SPDX-License-Identifier: AGPL-3.0-only
 * Optional inherited-pipe health reporting; no effect on ordinary injection.
 */
#ifndef NATARTOS_LOADER_BOOT_HEALTH_H
#define NATARTOS_LOADER_BOOT_HEALTH_H
#include "health.h"
#include "../boot_probe/raw_syscall.h"
static int boot_health_fd=-1;
static uint64_t boot_health_nonce;
/* A freestanding preload can run without a usable libc getenv. The kernel's
 * initial environment is available independently of libc initialization. */
static int boot_health_initial_env(char *buffer,unsigned capacity,const char **fdtext,const char **nonce) {
    long file=raw_open("/proc/self/environ",02000000);if(file<0)return -1;
    unsigned used=0;int result=-1;
    for(;;){
        if(used==capacity)break;
        long n=raw_call(NR_read,file,(long)(buffer+used),capacity-used,0,0);
        if(n==-4)continue;
        if(n<0)break;
        if(!n){result=0;break;}
        used+=(unsigned)n;
    }
    raw_close(file);if(result|| (used&&buffer[used-1]))return -1;
    const char *keys[]={"NATARTOS_BOOT_HEALTH_FD=","NATARTOS_BOOT_HEALTH_NONCE="};
    const char **values[]={fdtext,nonce};
    for(unsigned start=0;start<used;){
        unsigned end=start;while(end<used&&buffer[end])end++;
        for(unsigned k=0;k<2;k++){
            unsigned i=0;while(keys[k][i]&&start+i<end&&keys[k][i]==buffer[start+i])i++;
            if(!keys[k][i]){if(*values[k])return -1;*values[k]=buffer+start+i;}
        }
        start=end+1;
    }
    return 0;
}
static int boot_health_setup(char *(*getenv_fn)(const char *)) {
    const char *fdtext=getenv_fn?getenv_fn("NATARTOS_BOOT_HEALTH_FD"):0;
    const char *nonce=getenv_fn?getenv_fn("NATARTOS_BOOT_HEALTH_NONCE"):0;
    char initial[8192];
    if(!fdtext&&!nonce&&boot_health_initial_env(initial,sizeof(initial),&fdtext,&nonce))return -1;
    if(!fdtext&&!nonce)return 0;
    if(!fdtext||!nonce)return -1;
    unsigned fd=0,count=0;uint64_t value=0;
    while(*fdtext){if(*fdtext<'0'||*fdtext>'9'||++count>4)return -1;fd=fd*10+(unsigned)(*fdtext++-'0');}
    if(!count||fd<3||fd>4095)return -1;
    for(unsigned i=0;i<16;i++){
        unsigned char c=(unsigned char)nonce[i];unsigned digit;
        if(c>='0'&&c<='9')digit=c-'0';else if(c>='a'&&c<='f')digit=c-'a'+10;else return -1;
        value=(value<<4)|digit;
    }
    if(nonce[16]||!value)return -1;
    long flags=raw_call(NR_fcntl,fd,3 /* F_GETFL */,0,0,0);
    /* A nonblocking pipe is supplied by the supervisor. Never turn a regular
     * inherited file into a health output based on environment alone. */
    struct health_statx {uint32_t mask,blksize;uint64_t attributes;uint32_t nlink,uid,gid;uint16_t mode,pad;unsigned char rest[224];} st;
    _Static_assert(sizeof(st)==256,"health statx ABI");
    if(flags<0||raw_call(NR_statx,fd,(long)"",0x1000,3,(long)&st)<0)return -1;
    if((st.mask&3)!=3||(st.mode&0170000)!=0010000)return -1;
    if(raw_call(NR_fcntl,fd,2 /* F_SETFD */,1,0,0)<0||raw_call(NR_fcntl,fd,4 /* F_SETFL */,flags|04000,0,0)<0)return -1;
    boot_health_fd=(int)fd;boot_health_nonce=value;return 1;
}
static long boot_health_finish(int rendered) {
    if(boot_health_fd<0)return -1;
    long result=0;
    if(rendered){
        struct boot_health_message m={BOOT_HEALTH_MAGIC,boot_health_nonce,(uint32_t)raw_call(NR_getpid,0,0,0,0,0),1};
        result=raw_call(NR_write,boot_health_fd,(long)&m,sizeof(m),0,0);
    }
    raw_close(boot_health_fd);boot_health_fd=-1;return result;
}
#endif
