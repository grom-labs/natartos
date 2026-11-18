/* SPDX-License-Identifier: AGPL-3.0-only
 * Android-linker fixture with authored synchronous pthread and unavailable getenv.
 * Exercises the production loader and pipe ABI, not Bionic thread behavior.
 */
#include <stddef.h>
#include <stdint.h>
#include "health.h"
#include "../boot_probe/raw_syscall.h"
extern void *dlopen(const char *,int);
extern char *dlerror(void);
char *getenv(const char *name){(void)name;return NULL;}
int pthread_create(void *thread,const void *attr,void *(*function)(void *),void *arg){(void)thread;(void)attr;function(arg);return 0;}
__attribute__((noreturn)) static void finish(int result){
    const char *s=result?"ANDROID_BOOT_LOADER=FAIL\n":"ANDROID_BOOT_LOADER=PASS\n";size_t n=0;while(s[n])n++;
    raw_call(NR_write,1,(long)s,n,0,0);raw_call(NR_exit_group,result,0,0,0,0);for(;;){}
}
__attribute__((noreturn)) void _start(void){
    int pipes[2];if(raw_call(59 /* ARM64 pipe2 */,(long)pipes,04000|02000000,0,0,0)<0)finish(1);
    if(raw_call(NR_dup3,pipes[1],198,0,0,0)<0)finish(2);raw_close(pipes[1]);
    if(!dlopen("/vendor/natartos/natartos_loader.so",2)){
        const char *e=dlerror();if(e){size_t n=0;while(e[n])n++;raw_call(NR_write,1,(long)e,n,0,0);}finish(3);
    }
    struct boot_health_message m;long n=raw_call(NR_read,pipes[0],(long)&m,sizeof(m),0,0);raw_close(pipes[0]);
    if(n!=sizeof(m)||m.nonce!=UINT64_C(0x1234567890abcdef)||m.pid!=(uint32_t)raw_call(NR_getpid,0,0,0,0,0)||m.rendered!=1)finish(4);
    for(unsigned i=0;i<8;i++)if(m.magic[i]!=BOOT_HEALTH_MAGIC[i])finish(5);
    long fd=raw_open("/data/.natartos_status",0);char b[16];n=fd<0?-1:raw_call(NR_read,fd,(long)b,sizeof(b),0,0);if(fd>=0)raw_close(fd);
    const char expected[]="healthy";if(n!=7)finish(6);for(unsigned i=0;i<7;i++)if(b[i]!=expected[i])finish(7);
    finish(0);
}
