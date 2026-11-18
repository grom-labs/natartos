/* SPDX-License-Identifier: AGPL-3.0-only
 * Opt-in boot session supervisor. Fixed paths, exact profile, bounded startup.
 * Its pipe watcher restores the init service if this supervisor is killed.
 */
#define _GNU_SOURCE
#include "profile.h"
#include "health.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef NATARTOS_BOOT_TEST
#define BASE "/fixture"
#define STATE_DIR BASE "/state"
#define PROFILE BASE "/profile"
#define MANIFEST BASE "/manifest"
#define LOADER BASE "/natartos_loader.so"
#define MODS BASE "/mods"
#define ENABLED BASE "/enabled"
#define GUI BASE "/dji_glasses"
#define SETPROP BASE "/setprop"
#define GETPROP BASE "/getprop"
#define DISABLED BASE "/disabled"
#define BOOT_ID BASE "/boot_id"
#define LEGACY_PID BASE "/manual.pid"
#define RESTART BASE "/restart"
#define LEGACY_COUNT BASE "/legacy.count"
#define BOOT_ROOT BASE
#define INSTALL_ROOT BASE
#define STARTUP_DELAY_MS 200
#define HEALTH_MS 800
#define STOP_MS 800
#define RETRY_MS 50
#else
#define STATE_DIR "/data/dji/natartos-boot"
#define PROFILE "/vendor/lib64/natartos_boot/profile"
#define MANIFEST "/vendor/lib64/natartos_boot/manifest"
#define LOADER "/vendor/natartos/natartos_loader.so"
#define MODS "/vendor/natartos/mods"
#define ENABLED "/vendor/natartos/enabled"
#define GUI "/system/bin/dji_glasses"
#define SETPROP "/system/bin/setprop"
#define GETPROP "/system/bin/getprop"
#define DISABLED "/data/.natartos_disabled"
#define BOOT_ID "/proc/sys/kernel/random/boot_id"
#define LEGACY_PID "/data/local/tmp/natartos_inject.pid"
#define RESTART "/data/local/tmp/natartos_restart"
#define LEGACY_COUNT "/data/.natartos_bootcount"
#define BOOT_ROOT "/vendor/lib64/natartos_boot"
#define INSTALL_ROOT "/vendor/natartos"
#define STARTUP_DELAY_MS 10000
#define HEALTH_MS 30000
#define STOP_MS 8000
#define RETRY_MS 2000
#endif

static volatile sig_atomic_t stopping;
static int dirfd_=-1,lease=-1,logfd=-1;
static void on_signal(int s){(void)s;stopping=1;}
static uint64_t now_ms(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000+(unsigned long)t.tv_nsec/1000000;}
static void pause_ms(unsigned ms){struct timespec t={ms/1000,(long)(ms%1000)*1000000};while(nanosleep(&t,&t)&&errno==EINTR&&!stopping){}}
static void note(const char *format,...){
    char b[512];va_list ap;va_start(ap,format);int n=vsnprintf(b,sizeof(b),format,ap);va_end(ap);
    if(n<0)return;size_t len=(size_t)n;if(len>=sizeof(b)-1)len=sizeof(b)-2;b[len++]='\n';
    if(logfd>=0){struct stat st;if(!fstat(logfd,&st)&&st.st_size>=0&&(uint64_t)st.st_size+len<=131072)(void)write(logfd,b,len);}
    (void)write(2,b,len);
}
static void profile_error(const char *why){note("boot: %s",why);}
static int close_other_fds(int keep){
    DIR *d=opendir("/proc/self/fd");if(!d)return -1;int own=dirfd(d);
    struct dirent *ent;
    while((ent=readdir(d))){char *end;long fd=strtol(ent->d_name,&end,10);if(*end||fd<3||fd==keep||fd==own)continue;close((int)fd);}
    closedir(d);return 0;
}
static int present(const char *path){struct stat st;if(!lstat(path,&st))return 1;return errno==ENOENT?0:1;}
static int bypass(void){return stopping||present(DISABLED)||present(STATE_DIR "/disabled");}
static int trusted_dir(const char *p){
    struct stat st;int fd=open(p,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return -1;
    if(fstat(fd,&st)||st.st_uid||(st.st_mode&0022)){close(fd);return -1;}return fd;
}
static int enabled(void){
    int fd=boot_trusted_file(STATE_DIR "/enabled",O_RDONLY);if(fd<0)return 0;
    char b[32];ssize_t n=read(fd,b,sizeof(b));close(fd);
    return n==17&&!memcmp(b,"natartos-boot-v1\n",17);
}
static int manual_session(void){
    if(!present(LEGACY_PID))return 0;
    /* Advisory only: its contents can suppress startup, never authorize an
     * action on another PID. Legacy inject.sh often creates it mode 0666. */
    int fd=open(LEGACY_PID,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);struct stat st;
    if(fd<0)return 1;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid||st.st_size<1||st.st_size>31){close(fd);return 1;}
    char b[32]={0};ssize_t n=read(fd,b,sizeof(b)-1);close(fd);if(n<=0)return 1;
    char *end;long pid=strtol(b,&end,10);if(*end=='\n')end++;if(*end||pid<2||pid>4194304)return 1;
    return !kill((pid_t)pid,0)||errno!=ESRCH;
}
static int legacy_safe_mode(void){
    int fd=boot_trusted_file(LEGACY_COUNT,O_RDONLY);if(fd<0)return errno==ENOENT?0:1;
    unsigned char b[5];ssize_t n=read(fd,b,sizeof(b));close(fd);if(n!=4)return 1;
    uint32_t count=(uint32_t)b[0]|(uint32_t)b[1]<<8|(uint32_t)b[2]<<16|(uint32_t)b[3]<<24;
    return count>=5;
}
static int wait_owned(pid_t pid,int *status,unsigned timeout){
    uint64_t end=now_ms()+timeout;
    do{pid_t r=waitpid(pid,status,WNOHANG);if(r==pid)return 0;if(r<0&&errno!=EINTR)return -1;pause_ms(10);}while(now_ms()<end);
    return -1;
}
static int observe_child(pid_t pid){
    siginfo_t info;memset(&info,0,sizeof(info));
    if(waitid(P_PID,(id_t)pid,&info,WEXITED|WNOHANG|WNOWAIT))return -1;
    return info.si_pid==pid?1:0;
}
static int command_drain(int fd,char *output,size_t capacity,size_t *used,size_t *seen){
    char buf[256];ssize_t n;
    while((n=read(fd,buf,sizeof(buf)))>0){
        *seen+=(size_t)n;if(*seen>4096)return -1;
        if(output){if(*used+(size_t)n>=capacity)return -1;memcpy(output+*used,buf,(size_t)n);*used+=(size_t)n;}
    }
    return n<0&&errno!=EAGAIN&&errno!=EINTR?-1:0;
}
static int run_command(const char *path,const char *a,const char *b,char *output,size_t capacity){
    int fds[2];if(pipe2(fds,O_CLOEXEC|O_NONBLOCK))return -1;
    pid_t parent=getpid(),pid=fork();if(pid<0){close(fds[0]);close(fds[1]);return -1;}
    if(!pid){
        if(prctl(PR_SET_PDEATHSIG,SIGKILL)||getppid()!=parent)_exit(126);
        dup2(fds[1],1);int null=open("/dev/null",O_RDWR);if(null>=0){dup2(null,0);dup2(null,2);}
        char *const args[]={(char *)path,(char *)a,(char *)b,NULL};
        char *const env[]={"PATH=/system/bin:/vendor/bin:/bin",NULL};execve(path,args,env);_exit(127);
    }
    close(fds[1]);int status=0,finished=0,bad=0,owned=1;size_t used=0,seen=0;uint64_t deadline=now_ms()+3000;
    while(now_ms()<deadline){
        if(command_drain(fds[0],output,capacity,&used,&seen)){bad=1;kill(pid,SIGKILL);}
        pid_t r=waitpid(pid,&status,WNOHANG);if(r==pid){finished=1;owned=0;break;}
        if(r<0&&errno!=EINTR){owned=0;break;}pause_ms(10);
    }
    if(finished&&command_drain(fds[0],output,capacity,&used,&seen))bad=1;
    close(fds[0]);if(output&&capacity)output[used]=0;
    if(!finished){if(owned){kill(pid,SIGKILL);(void)wait_owned(pid,&status,500);}return -1;}
    return !bad&&WIFEXITED(status)&&!WEXITSTATUS(status)?0:-1;
}
static int service_wait(const char *state){
    uint64_t deadline=now_ms()+STOP_MS;
    do{char out[64];if(!run_command(GETPROP,"init.svc.dji_glasses",NULL,out,sizeof(out))){size_t n=strcspn(out,"\r\n");out[n]=0;if(!strcmp(out,state))return 0;}pause_ms(50);}while(now_ms()<deadline);
    return -1;
}
static int stock_restore(void){
    int a=run_command(SETPROP,"dji.glasses_service","0",NULL,0);int b=service_wait("stopped");
    int c=run_command(SETPROP,"dji.glasses_service","1",NULL,0);int d=service_wait("running");
    note("boot: stock restore %s",a||b||c||d?"UNCONFIRMED":"confirmed");return a||b||c||d?-1:0;
}
static void watcher_mark_failure(void);
static pid_t watcher_start(int *writer){
    int fds[2];if(pipe2(fds,O_CLOEXEC))return -1;
    pid_t pid=fork();if(pid<0){close(fds[0]);close(fds[1]);return -1;}
    if(!pid){
        close(fds[1]);if(setsid()<0)_exit(1);signal(SIGTERM,SIG_IGN);signal(SIGINT,SIG_IGN);
        if(close_other_fds(fds[0]))_exit(1);logfd=boot_trusted_file(STATE_DIR "/boot.log",O_WRONLY|O_APPEND);
        int armed=0;char c;ssize_t n;
        while((n=read(fds[0],&c,1))!=0){if(n<0){if(errno==EINTR)continue;_exit(1);}if(c=='A')armed=1;}
        close(fds[0]);stopping=0;
        if(armed){watcher_mark_failure();_exit(stock_restore()?1:0);}_exit(0);
    }
    close(fds[0]);*writer=fds[1];return pid;
}
struct state {char boot[37],phase[16];unsigned failures;};
static int valid_boot(const char *b){
    if(strlen(b)!=36)return 0;
    for(unsigned i=0;i<36;i++){if(i==8||i==13||i==18||i==23){if(b[i]!='-')return 0;}else if(!strchr("0123456789abcdef",b[i]))return 0;}return 1;
}
static int read_boot_id(char id[40]){
    memset(id,0,40);int fd=open(BOOT_ID,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);ssize_t n=fd<0?-1:read(fd,id,39);if(fd>=0)close(fd);
    if(n<=0||n>=39)return -1;id[strcspn(id,"\r\n")]=0;return valid_boot(id)?0:-1;
}
static int state_read(struct state *s){
    memset(s,0,sizeof(*s));int fd=boot_trusted_file(STATE_DIR "/state",O_RDONLY);
    if(fd<0)return errno==ENOENT?0:-1;
    char b[128];ssize_t n=read(fd,b,sizeof(b)-1);close(fd);if(n<=0||b[n-1]!='\n'||memchr(b,0,(size_t)n))return -1;b[n]=0;
    unsigned version;int used=0;
    if(sscanf(b,"%u %36s %u %15s\n%n",&version,s->boot,&s->failures,s->phase,&used)!=4||used!=n||version!=1||s->failures>3||!valid_boot(s->boot))return -1;
    return !strcmp(s->phase,"pending")||!strcmp(s->phase,"healthy")||!strcmp(s->phase,"failed")?0:-1;
}
static int state_write(const struct state *s){
    char name[64],b[128];snprintf(name,sizeof(name),".state.%ld",(long)getpid());
    int fd=openat(dirfd_,name,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)return -1;
    int n=snprintf(b,sizeof(b),"1 %s %u %s\n",s->boot,s->failures,s->phase);
    int rc=write(fd,b,(size_t)n)==n&&fsync(fd)==0?0:-1;close(fd);
    if(!rc)rc=renameat(dirfd_,name,dirfd_,"state");if(!rc)rc=fsync(dirfd_);
    if(rc)unlinkat(dirfd_,name,0);return rc;
}
static void watcher_mark_failure(void){
    /* A dead supervisor has released its lease. A normal live supervisor has
     * already saved failure and still owns it; do not compete with that owner. */
    dirfd_=trusted_dir(STATE_DIR);int fd=boot_trusted_file(STATE_DIR "/lease",O_RDWR);
    if(dirfd_>=0&&fd>=0){
        uint64_t end=now_ms()+500;
        do{
            if(!flock(fd,LOCK_EX|LOCK_NB)){
                struct state s;if(!state_read(&s)&&s.boot[0]){if(!s.failures)s.failures=1;strcpy(s.phase,"failed");(void)state_write(&s);}
                break;
            }
            if(errno!=EAGAIN&&errno!=EWOULDBLOCK)break;
            /* Pipe EOF can precede deferred release of the parent's lease.
             * A normal live parent has already persisted its failure. */
            struct state current;if(!state_read(&current)&&current.failures&&!strcmp(current.phase,"failed"))break;
            pause_ms(10);
        }while(now_ms()<end);
    }
    if(fd>=0)close(fd);if(dirfd_>=0)close(dirfd_);dirfd_=-1;
}
static int simple_name(const char *n){size_t l=strlen(n);return l>3&&l<100&&!strcmp(n+l-3,".so")&&strspn(n,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-")==l&&n[0]!='.';}
static int install_preload(char preload[4096]){
    int fd=trusted_dir(INSTALL_ROOT);if(fd<0)return -1;close(fd);
    fd=trusted_dir(MODS);if(fd<0)return -1;close(fd);fd=trusted_dir(ENABLED);if(fd<0)return -1;close(fd);
    char names[16][100];unsigned count=0;DIR *d=opendir(ENABLED);if(!d)return -1;
    struct dirent *entry;
    while((entry=readdir(d))){
        if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
        if(count==16||!simple_name(entry->d_name)){closedir(d);return -1;}
        char p[512];snprintf(p,sizeof(p),"%s/%s",ENABLED,entry->d_name);fd=boot_trusted_file(p,O_RDONLY);if(fd<0){closedir(d);return -1;}close(fd);
        strcpy(names[count++],entry->d_name);
    }closedir(d);if(!count)return -1;
    for(unsigned i=0;i<count;i++)for(unsigned j=i+1;j<count;j++)if(strcmp(names[i],names[j])>0){char tmp[100];strcpy(tmp,names[i]);strcpy(names[i],names[j]);strcpy(names[j],tmp);}
    int has_overlay=0;for(unsigned i=0;i<count;i++)if(!strcmp(names[i],"hook_overlay.so"))has_overlay=1;if(!has_overlay)return -1;
    fd=boot_trusted_file(MANIFEST,O_RDONLY);if(fd<0)return -1;FILE *manifest=fdopen(fd,"r");if(!manifest){close(fd);return -1;}
    preload[0]=0;char line[640];int rc=-1;
    for(unsigned i=0;i<=count;i++){
        char path[512],expected[640],hash[65];
        if(i)snprintf(path,sizeof(path),"%s/%s",MODS,names[i-1]);else strcpy(path,LOADER);
        if(!fgets(line,sizeof(line),manifest)||strlen(line)<67||strspn(line,"0123456789abcdef")!=64)goto done;
        snprintf(expected,sizeof(expected)," %s\n",path);if(strcmp(line+64,expected))goto done;
        fd=boot_trusted_file(path,O_RDONLY);if(fd<0)goto done;
        struct stat st;int ok=!fstat(fd,&st)&&st.st_size>0&&st.st_size<=33554432&&!boot_sha256_fd(fd,(uint64_t)st.st_size,hash);close(fd);
        if(!ok||memcmp(line,hash,64))goto done;
        size_t used=strlen(preload),need=strlen(path);if(used+need+2>=4096)goto done;
        if(used)preload[used++]=':';strcpy(preload+used,path);
    }
    if(fgetc(manifest)!=EOF||ferror(manifest))goto done;rc=0;
done:fclose(manifest);return rc;
}
static int stop_gui(pid_t pid){
    if(pid<=1)return 0;if(observe_child(pid)<0)return -1;
    kill(-pid,SIGTERM);uint64_t deadline=now_ms()+1000;int status;
    while(now_ms()<deadline){int exited=observe_child(pid);if(exited<0)return -1;if(exited)break;pause_ms(20);}
    /* WNOWAIT keeps the child PID reserved until the final group signal. */
    kill(-pid,SIGKILL);return wait_owned(pid,&status,1000);
}
static int gui_session(const char *preload,int watchfd,pid_t watcher,struct state *s){
    int pipes[2];if(pipe2(pipes,O_CLOEXEC|O_NONBLOCK))return -1;
    uint64_t nonce=0;if(getrandom(&nonce,sizeof(nonce),GRND_NONBLOCK)!=sizeof(nonce)||!nonce){close(pipes[0]);close(pipes[1]);note("boot: health nonce unavailable");return -1;}
    char ld[4120],nonce_env[64];snprintf(ld,sizeof(ld),"LD_PRELOAD=%s",preload);snprintf(nonce_env,sizeof(nonce_env),"NATARTOS_BOOT_HEALTH_NONCE=%016" PRIx64,nonce);
    pid_t parent=getpid(),pid=fork();
    if(!pid){
        if(setpgid(0,0)||prctl(PR_SET_PDEATHSIG,SIGKILL)||getppid()!=parent)_exit(126);
        if(bypass())_exit(126);signal(SIGPIPE,SIG_DFL);
        close(watchfd);close(pipes[0]);if(dup2(pipes[1],198)<0)_exit(126);if(pipes[1]!=198)close(pipes[1]);fcntl(198,F_SETFD,0);
        char *const args[]={GUI,"-g","-f",NULL};
        char *const env[]={"PATH=/sbin:/system/sbin:/system/bin:/system/xbin",
            "DOWNLOAD_CACHE=/data/cache","ANDROID_BOOTLOGO=1","ANDROID_ROOT=/system",
            "ANDROID_ASSETS=/system/app","ANDROID_DATA=/data","ANDROID_STORAGE=/storage",
            "EXTERNAL_STORAGE=/sdcard","ASEC_MOUNTPOINT=/mnt/asec",
            ld,"NATARTOS_BOOT_HEALTH_FD=198",nonce_env,NULL};
        execve(GUI,args,env);_exit(127);
    }
    close(pipes[1]);if(pid<0){close(pipes[0]);return -1;}setpgid(pid,pid);
    note("boot: GUI started pid=%ld",(long)pid);uint64_t deadline=now_ms()+HEALTH_MS;
    struct boot_health_message message;size_t used=0;int healthy=0,result=-1;
    for(;;){
        if(bypass()||waitpid(watcher,NULL,WNOHANG)!=0){note("boot: bypass or watcher lost");break;}
        int exited=observe_child(pid);
        if(exited){if(exited>0){note("boot: GUI exited");result=1;}else note("boot: GUI wait failed");break;}
        if(!healthy){
            ssize_t n=read(pipes[0],(char *)&message+used,sizeof(message)-used);
            if(n>0)used+=(size_t)n;
            else if(n==0||(n<0&&errno!=EAGAIN&&errno!=EINTR)){note("boot: health channel closed");break;}
            if(used==sizeof(message)){
                if(memcmp(message.magic,BOOT_HEALTH_MAGIC,8)||message.nonce!=nonce||message.pid!=(uint32_t)pid||message.rendered!=1){note("boot: invalid render health");break;}
                healthy=1;strcpy(s->phase,"healthy");s->failures=0;
                if(state_write(s)){note("boot: cannot persist healthy state");break;}
                note("boot: render health confirmed");
            }else if(now_ms()>=deadline){note("boot: render health timed out");break;}
        }
        pause_ms(20);
    }
    close(pipes[0]);if(stop_gui(pid)){note("boot: GUI termination unconfirmed");result=-1;}return result;
}

int main(int argc,char **argv){
    int session=argc==2&&!strcmp(argv[1],"--session");
    if(!session&&(argc!=2||strcmp(argv[1],"--boot"))){fprintf(stderr,"usage: boot-supervisor --boot | --session\n");return 2;}
    if(geteuid()||close_other_fds(-1))return 1;
    sigset_t empty;sigemptyset(&empty);if(sigprocmask(SIG_SETMASK,&empty,NULL))return 1;
    signal(SIGCHLD,SIG_DFL);
    signal(SIGTERM,on_signal);signal(SIGINT,on_signal);signal(SIGPIPE,SIG_IGN);
    if(bypass()||!enabled()){note("boot: disabled/bypassed");return 0;}
    dirfd_=trusted_dir(STATE_DIR);if(dirfd_<0)return 1;
    lease=openat(dirfd_,"lease",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);struct stat st;
    if(lease<0||fstat(lease,&st)||!S_ISREG(st.st_mode)||st.st_uid||(st.st_mode&0022)||flock(lease,LOCK_EX|LOCK_NB))return 1;
    logfd=boot_trusted_file(STATE_DIR "/boot.log",O_WRONLY|O_APPEND);
    boot_profile_set_logger(profile_error);
#ifndef NATARTOS_BOOT_TEST
    if(!session){char context[64]={0};int fd=open("/proc/self/attr/current",O_RDONLY|O_CLOEXEC);ssize_t n=fd<0?-1:read(fd,context,sizeof(context)-1);if(fd>=0)close(fd);
        context[strcspn(context,"\r\n")]=0;
        if(n<=0||strcmp(context,"u:r:dji_services:s0")){note("boot: unexpected execution context");return 1;}}
#endif
    uint64_t end=now_ms()+STARTUP_DELAY_MS;
    do{if(bypass()){note("boot: startup bypass");return 0;}pause_ms(50);}while(now_ms()<end);
    struct boot_profile profile;struct permission_check permission;
    int rootfd=trusted_dir(BOOT_ROOT);if(rootfd<0){note("boot: untrusted profile directory");return 1;}close(rootfd);
    if(boot_profile_load(PROFILE,&profile)||boot_permission_check(&profile,&permission)){note("boot: permission profile/check failed");return 1;}
    char id[40];if(read_boot_id(id)){note("boot: invalid boot ID");return 1;}
#ifdef NATARTOS_BOOT_QUALIFY
    note("BOOT_QUALIFY_OK mode=%s boot_id=%s pid=%ld enforcing=%d word=0x%08" PRIx32 " memory_writes=0 ui_changes=0",session?"session":"boot",id,(long)getpid(),permission.enforcing,permission.word);
    return 0;
#else
    if(manual_session()||legacy_safe_mode()){note("boot: manual session or legacy safe mode");return 1;}
    if(session&&permission.enforcing){note("boot: session mode requires already-permissive state");return 1;}
    char preload[4096];if(install_preload(preload)){note("boot: installation manifest/modes mismatch");return 1;}
    struct state s;if(state_read(&s)){note("boot: corrupt state; manual reset required");return 1;}
    if(!strcmp(id,s.boot)||s.failures>=3){note("boot: already attempted this boot or failure limit reached");return 0;}
    note("boot: activation preflight mode=%s boot_id=%s enforcing=%d word=0x%08" PRIx32,session?"session":"boot",id,permission.enforcing,permission.word);
    strcpy(s.boot,id);s.failures++;strcpy(s.phase,"pending");
    if(bypass())return 1;
    if(state_write(&s)){note("boot: cannot persist activation attempt");return 1;}
    if(!session&&boot_permission_apply(&profile,bypass)){note("boot: permission preflight/transition failed");return 1;}
    if(boot_permission_inspect(profile.state_phys,&permission)||permission.enforcing||permission.word!=0x01000000U){note("boot: permissive state unconfirmed before GUI startup");return 1;}
    note("boot: permission ready mode=%s boot_id=%s enforcing=0 word=0x01000000",session?"session":"boot",id);
    if(bypass()||install_preload(preload))return 1;
    int writer;pid_t watcher=watcher_start(&writer);if(watcher<0)return 1;
    /* Arm before stopping stock so a killed supervisor cannot strand that step. */
    if(write(writer,"A",1)!=1){close(writer);int status;(void)wait_owned(watcher,&status,1000);return 1;}
    int result=-1;
    if(!run_command(SETPROP,"dji.glasses_service","0",NULL,0)&&!service_wait("stopped")){
        uint64_t window=now_ms();unsigned attempts=0;
        while(!bypass()){
            if(now_ms()-window>=120000){window=now_ms();attempts=0;}
            if(attempts++>=3){note("boot: restart limit reached");break;}
#ifndef NATARTOS_BOOT_TEST
            unlink("/data/local/tmp/fb_shared");unlink("/data/local/tmp/fb_stop");unlink("/data/local/tmp/fb_override");
#endif
            unlink(RESTART);
            if(install_preload(preload)){note("boot: installation drift");break;}
            result=gui_session(preload,writer,watcher,&s);if(result<0)break;
            /* Intentional and crash restarts both consume the bounded budget. */
            unlink(RESTART);pause_ms(RETRY_MS);
        }
    }else note("boot: stock service stop unconfirmed");
    strcpy(s.phase,"failed");if(!s.failures)s.failures=1;(void)state_write(&s);
    close(writer);int status=0;int r=wait_owned(watcher,&status,2*STOP_MS+16000);
    if(r<0){if(observe_child(watcher)>=0){kill(watcher,SIGKILL);(void)wait_owned(watcher,&status,500);}}
    if(r<0||!WIFEXITED(status)||WEXITSTATUS(status)){stopping=0;stock_restore();}
    note("boot: session ended; permissive state, if entered, lasts until reboot");return 1;
#endif
}
