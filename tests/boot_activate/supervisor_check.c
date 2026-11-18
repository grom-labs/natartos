/* SPDX-License-Identifier: AGPL-3.0-only */
#define _GNU_SOURCE
#undef NDEBUG
#include "profile.h"
#include "loader_health.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static const char *supervisor;
static unsigned checks;
static int ignored_chld;
static void pause_ms(unsigned ms){struct timespec t={ms/1000,(long)(ms%1000)*1000000};nanosleep(&t,NULL);}
static void put(const char *p,const void *b,size_t n){int fd=open(p,O_WRONLY|O_CREAT|O_TRUNC,0600);assert(fd>=0);assert(write(fd,b,n)==(ssize_t)n);close(fd);}
static void text(const char *p,const char *s){put(p,s,strlen(s));}
static void copy(const char *src,const char *dst){
    int in=open(src,O_RDONLY);assert(in>=0);int out=open(dst,O_WRONLY|O_CREAT|O_TRUNC,0700);assert(out>=0);char b[4096];ssize_t n;
    while((n=read(in,b,sizeof(b)))>0)assert(write(out,b,(size_t)n)==n);assert(n==0);close(in);close(out);
}
static int contains(const char *p,const char *s){char b[1024];int fd=open(p,O_RDONLY);if(fd<0)return 0;ssize_t n=read(fd,b,sizeof(b)-1);close(fd);if(n<0)return 0;b[n]=0;return strstr(b,s)!=NULL;}
static unsigned starts(void){unsigned n=0;FILE *f=fopen("/fixture/starts","r");if(f){int c;while((c=fgetc(f))!=EOF)if(c=='\n')n++;fclose(f);}return n;}
static pid_t spawn(void){
    pid_t p=fork();assert(p>=0);if(!p){
        if(ignored_chld){signal(SIGCHLD,SIG_IGN);sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGCHLD);sigaddset(&mask,SIGTERM);sigprocmask(SIG_BLOCK,&mask,NULL);}
        execl(supervisor,supervisor,"--session",NULL);_exit(127);
    }return p;
}
static int finish(pid_t p){
    int status;for(unsigned i=0;i<800;i++){pid_t n=waitpid(p,&status,WNOHANG);if(n==p)return status;assert(n==0||errno==EINTR);pause_ms(10);}
    kill(p,SIGKILL);waitpid(p,&status,0);assert(!"supervisor exceeded timeout");return status;
}
static void wait_for(const char *path,const char *needle){for(unsigned i=0;i<300;i++){if(contains(path,needle))return;pause_ms(10);}assert(!"expected state timed out");}
static void manifest(void){
    const char *paths[]={"/fixture/natartos_loader.so","/fixture/mods/hook_overlay.so"};
    FILE *f=fopen("/fixture/manifest","w");assert(f);assert(!chmod("/fixture/manifest",0600));
    for(unsigned i=0;i<2;i++){int fd=open(paths[i],O_RDONLY);assert(fd>=0);struct stat st;assert(!fstat(fd,&st));char hex[65];assert(!boot_sha256_fd(fd,(uint64_t)st.st_size,hex));close(fd);fprintf(f,"%s %s\n",hex,paths[i]);}assert(!fclose(f));
}
static void reset(void){
    unlink("/fixture/state/state");unlink("/fixture/state/disabled");unlink("/fixture/disabled");unlink("/fixture/manual.pid");unlink("/fixture/legacy.count");unlink("/fixture/starts");unlink("/fixture/flood");
    text("/fixture/service","running\n");text("/fixture/state/boot.log","");text("/fixture/state/enabled","natartos-boot-v1\n");chmod("/fixture/state/enabled",0600);
    text("/fixture/mode","healthy");text("/fixture/enforce","0");text("/fixture/normal","abc");text("/fixture/natartos_loader.so","loader");
    text("/fixture/enabled/hook_overlay.so","");chmod("/fixture/enabled/hook_overlay.so",0600);
    text("/fixture/mods/hook_overlay.so","overlay");chmod("/fixture/mods/hook_overlay.so",0600);manifest();
    text("/fixture/boot_id","aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa\n");
    uint32_t w=0x01000000;int fd=open("/fixture/memory",O_RDWR);assert(fd>=0);assert(pwrite(fd,&w,4,0x80000)==4);close(fd);
}
static void skipped(void){int status=finish(spawn());assert(WIFEXITED(status));assert(!starts());assert(contains("/fixture/service","running"));checks++;}
static void failed_ui(const char *mode){reset();text("/fixture/mode",mode);int status=finish(spawn());assert(WIFEXITED(status)&&WEXITSTATUS(status));assert(starts()>0&&starts()<=3);assert(contains("/fixture/service","running"));assert(contains("/fixture/state/state","failed"));checks++;}
int main(int argc,char **argv){
    assert(argc==5);supervisor=argv[1];alarm(45);
    mkdir("/fixture",0700);assert(!mkdir("/fixture/state",0700));assert(!mkdir("/fixture/mods",0700));assert(!mkdir("/fixture/enabled",0700));
    copy(argv[2],"/fixture/dji_glasses");copy(argv[3],"/fixture/setprop");copy(argv[3],"/fixture/getprop");
    int fd=open("/fixture/memory",O_CREAT|O_RDWR,0600);assert(fd>=0);assert(!ftruncate(fd,0x80008));close(fd);
    struct utsname un;assert(!uname(&un));char b[1024];
    snprintf(b,sizeof(b),"schema=1\nrelease=%s\nversion=%s\nslot=1\nimage_sha256=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\nstate_phys=0x80000\n",un.release,un.version);text("/fixture/profile",b);
    text("/fixture/slot","androidboot.slot_suffix=1\n");
    /* The same reporting code used by the loader rejects forged destinations. */
    unsetenv("NATARTOS_BOOT_HEALTH_FD");unsetenv("NATARTOS_BOOT_HEALTH_NONCE");assert(!boot_health_setup(getenv));
    setenv("NATARTOS_BOOT_HEALTH_FD","1",1);setenv("NATARTOS_BOOT_HEALTH_NONCE","1234567890abcdef",1);assert(boot_health_setup(getenv)<0);
    fd=open("/fixture/not-a-pipe",O_CREAT|O_RDWR,0600);assert(fd>=3);char fdstr[32];snprintf(fdstr,sizeof(fdstr),"%d",fd);setenv("NATARTOS_BOOT_HEALTH_FD",fdstr,1);assert(boot_health_setup(getenv)<0);close(fd);
    unsetenv("NATARTOS_BOOT_HEALTH_FD");unsetenv("NATARTOS_BOOT_HEALTH_NONCE");boot_health_finish(0);checks++;
    reset();supervisor=argv[4];int qualifier_status=finish(spawn());assert(WIFEXITED(qualifier_status)&&!WEXITSTATUS(qualifier_status));assert(!starts());checks++;
    reset();uint32_t enforcing_word=0x01000100;
    fd=open("/fixture/memory",O_RDWR);assert(pwrite(fd,&enforcing_word,4,0x80000)==4);close(fd);text("/fixture/enforce","1");
    qualifier_status=finish(spawn());assert(WIFEXITED(qualifier_status)&&!WEXITSTATUS(qualifier_status));assert(!starts());assert(contains("/fixture/service","running"));
    uint32_t unchanged;fd=open("/fixture/memory",O_RDONLY);assert(pread(fd,&unchanged,4,0x80000)==4);close(fd);assert(unchanged==enforcing_word);checks++;
    supervisor=argv[1];
    reset();unlink("/fixture/state/enabled");skipped();
    reset();text("/fixture/disabled","");skipped();
    reset();text("/fixture/state/disabled","");skipped();
    reset();chmod("/fixture/state/enabled",0666);skipped();
    reset();text("/fixture/state/state","corrupt\n");skipped();
    reset();text("/fixture/state/state","1 bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb 3 failed\n");skipped();
    reset();text("/fixture/normal","changed");skipped();assert(contains("/fixture/state/boot.log","boot image digest mismatch"));
    reset();text("/fixture/natartos_loader.so","changed");skipped();
    reset();chmod("/fixture/enabled/hook_overlay.so",0666);skipped();
    reset();text("/fixture/manual.pid","1\n");skipped();
    reset();uint32_t legacy=5;put("/fixture/legacy.count",&legacy,sizeof(legacy));skipped();
    reset();text("/fixture/enforce","1");skipped();
    failed_ui("crash");failed_ui("hang");failed_ui("badnonce");failed_ui("eof");
    reset();text("/fixture/flood","");skipped();
    reset();text("/fixture/manual.pid","4194304\n");chmod("/fixture/manual.pid",0666);text("/fixture/mode","ignoreterm");ignored_chld=1;
    pid_t p=spawn();wait_for("/fixture/state/state","healthy");assert(starts()==1);ignored_chld=0;
    /* Duplicate supervisor cannot stop or replace the live session. */
    int status=finish(spawn());assert(WIFEXITED(status)&&WEXITSTATUS(status));assert(starts()==1);checks++;
    text("/fixture/state/disabled","");status=finish(p);assert(WIFEXITED(status));assert(contains("/fixture/service","running"));checks++;
    unlink("/fixture/state/disabled");unsigned before=starts();status=finish(spawn());assert(starts()==before);checks++;
    for(unsigned repeat=0;repeat<5;repeat++){
        reset();p=spawn();wait_for("/fixture/state/state","healthy");kill(p,SIGKILL);status=finish(p);assert(WIFSIGNALED(status));wait_for("/fixture/service","running");wait_for("/fixture/state/state","failed");
        /* A killed attempt blocks another run in the same boot. */
        before=starts();status=finish(spawn());assert(starts()==before);
    }
    checks+=2;
    printf("BOOT_SUPERVISOR_TESTS_PASSED=%u\n",checks);return 0;
}
