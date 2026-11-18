/* SPDX-License-Identifier: AGPL-3.0-only
 * The fork children use raw syscalls only, until exec of the static helper.
 * No allocation, libc, dynamic linking or inherited-lock use after fork.
 */
#ifndef NATARTOS_BOOT_TRIGGER_H
#define NATARTOS_BOOT_TRIGGER_H
#include <stddef.h>
#include <stdint.h>
#include "../boot_probe/raw_syscall.h"
#ifdef NATARTOS_BOOT_TRIGGER_TEST
#define TRIGGER_BASE "/trigger"
#define TRIGGER_HELPER TRIGGER_BASE "/helper"
#define TRIGGER_ENABLED TRIGGER_BASE "/enabled"
#define TRIGGER_DISABLED TRIGGER_BASE "/disabled"
#else
#define TRIGGER_BASE "/vendor/lib64/natartos_boot"
#define TRIGGER_HELPER TRIGGER_BASE "/boot-supervisor"
#define TRIGGER_ENABLED "/data/dji/natartos-boot/enabled"
#define TRIGGER_DISABLED "/data/.natartos_disabled"
#endif
struct trigger_statx {
    uint32_t mask,blksize;uint64_t attributes;uint32_t nlink,uid,gid;
    uint16_t mode,pad;unsigned char rest[224];
};
_Static_assert(sizeof(struct trigger_statx)==256,"trigger statx ABI");
static int trigger_trusted(const char *path,unsigned type){
    struct trigger_statx st;
    long rc=raw_call(NR_statx,-100,(long)path,0x100 /* nofollow */,11,(long)&st);
    return !rc&&(st.mask&11)==11&&!st.uid&&!(st.mode&0022)&&(st.mode&0170000)==type;
}
static int trigger_absent(const char *path){
    struct trigger_statx st;return raw_call(NR_statx,-100,(long)path,0x100,1,(long)&st)==-2;
}
static int trigger_identity(void){
#ifdef NATARTOS_BOOT_TRIGGER_TEST
    return 1;
#else
    char b[64];long fd=raw_open("/proc/self/cmdline",02000000|RAW_O_NOFOLLOW);if(fd<0)return 0;
    long n=raw_call(NR_read,fd,(long)b,sizeof(b),0,0);raw_close(fd);
    const char expected[]="/system/bin/dji_sys";
    if(n<(long)sizeof(expected))return 0;
    for(unsigned i=0;i<sizeof(expected);i++)if(b[i]!=expected[i])return 0;
    return 1;
#endif
}
static void boot_activation_start(void){
    if(raw_call(NR_geteuid,0,0,0,0,0)||!trigger_identity()||
       !trigger_absent(TRIGGER_DISABLED)||!trigger_trusted(TRIGGER_BASE,0040000)||
       !trigger_trusted(TRIGGER_HELPER,0100000)||!trigger_trusted(TRIGGER_ENABLED,0100000))return;
    char b[32];long fd=raw_open(TRIGGER_ENABLED,02000000|RAW_O_NOFOLLOW|04000);if(fd<0)return;
    long n=raw_call(NR_read,fd,(long)b,sizeof(b),0,0);raw_close(fd);
    const char enabled[]="natartos-boot-v1\n";
    if(n!=(long)sizeof(enabled)-1)return;
    for(unsigned i=0;i<sizeof(enabled)-1;i++)if(b[i]!=enabled[i])return;
    long pid=raw_call(NR_clone,17 /* SIGCHLD, fork semantics */,0,0,0,0);
    if(pid<0)return;
    if(!pid){
        if(raw_call(NR_setsid,0,0,0,0,0)<0)goto child_exit;
        long detached=raw_call(NR_clone,17,0,0,0,0);
        if(detached!=0)goto child_exit;
        long nullfd=raw_open("/dev/null",2|02000000);
        if(nullfd<0)goto child_exit;
        if(raw_call(NR_fcntl,nullfd,2 /* F_SETFD */,0,0,0)<0)goto child_exit;
        for(long i=0;i<3;i++)if(nullfd!=i)(void)raw_call(NR_dup3,nullfd,i,0,0,0);
        if(nullfd>2)raw_close(nullfd);
        const char *args[]={TRIGGER_HELPER,"--boot",NULL};
        const char *env[]={"PATH=/system/bin:/vendor/bin",NULL};
        (void)raw_call(NR_execve,(long)TRIGGER_HELPER,(long)args,(long)env,0,0);
child_exit:
        (void)raw_call(NR_exit_group,0,0,0,0,0);
        __builtin_unreachable();
    }
    /* Reap only the short handoff child, never wait for boot activation. */
    long rc;do{rc=raw_call(NR_wait4,pid,0,0,0,0);}while(rc==-4);
}
#endif
