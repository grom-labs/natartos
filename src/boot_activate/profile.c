/* SPDX-License-Identifier: AGPL-3.0-only
 * Experimental exact-build permission profile. No address scanning or fallback.
 */
#define _GNU_SOURCE
#include "profile.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <unistd.h>

#ifdef NATARTOS_BOOT_TEST
#define ENFORCE "/fixture/enforce"
#define MEMORY "/fixture/memory"
#define SLOT "/fixture/slot"
#define IMAGE1 "/fixture/normal"
#define IMAGE2 "/fixture/normal_2"
#else
#define ENFORCE "/sys/fs/selinux/enforce"
#define MEMORY "/dev/mem"
#define SLOT "/proc/cmdline"
#define IMAGE1 "/dev/block/by-name/normal"
#define IMAGE2 "/dev/block/by-name/normal_2"
#endif

static void (*error_logger)(const char *);
void boot_profile_set_logger(void (*logger)(const char *)) { error_logger=logger; }
static int fail(const char *why) {
    if(error_logger)error_logger(why);else fprintf(stderr,"boot: %s\n",why);
    return -1;
}

int boot_trusted_file(const char *path, int flags) {
    int fd = open(path, flags | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat st;
    if (fd < 0) return -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid || (st.st_mode & 0022)) {
        close(fd); errno = EPERM; return -1;
    }
    return fd;
}

int boot_profile_parse(const char *text, size_t size, struct boot_profile *out) {
    char buf[1024]; unsigned seen = 0;
    if (!size || size >= sizeof(buf) || text[size-1] != '\n' || memchr(text, 0, size)) return -1;
    memcpy(buf, text, size); buf[size] = 0; memset(out, 0, sizeof(*out));
    char *line = buf;
    while (*line) {
        char *end = strchr(line, '\n');
        if (!end || end == line) return -1;
        *end = 0;
        char *value = strchr(line, '=');
        if (!value || !value[1]) return -1;
        *value++ = 0;
        unsigned bit;
        if (!strcmp(line, "schema")) { bit=1; if (strcmp(value, "1")) return -1; }
        else if (!strcmp(line, "release")) {
            bit=2; if (strlen(value)>=sizeof(out->release)) return -1; strcpy(out->release,value);
        } else if (!strcmp(line, "version")) {
            bit=4; if (strlen(value)>=sizeof(out->version)) return -1; strcpy(out->version,value);
        } else if (!strcmp(line, "slot")) {
            bit=8; if (strcmp(value,"1") && strcmp(value,"2")) return -1; out->slot=(unsigned)(value[0]-'0');
        } else if (!strcmp(line, "image_sha256")) {
            bit=16; if (strlen(value)!=64 || strspn(value,"0123456789abcdef")!=64) return -1;
            strcpy(out->image_sha256,value);
        } else if (!strcmp(line, "state_phys")) {
            bit=32;
            if (strncmp(value,"0x",2) || !value[2] || strlen(value)>18 ||
                strspn(value+2,"0123456789abcdef")!=strlen(value+2)) return -1;
            errno=0; out->state_phys=strtoull(value+2,NULL,16);
            /* Only aligned normal-RAM addresses; never an arbitrary MMIO probe. */
            if (errno || out->state_phys < 0x80000 || out->state_phys > 0x3b9ffff8 || (out->state_phys & 7)) return -1;
        } else return -1;
        if (seen & bit) return -1;
        seen |= bit; line=end+1;
    }
    return seen == 63 ? 0 : -1;
}

int boot_profile_load(const char *path, struct boot_profile *out) {
    int fd=boot_trusted_file(path,O_RDONLY); char text[1024];
    if (fd<0) return fail("profile unavailable/untrusted");
    size_t used=0;ssize_t n=0;
    while(used<sizeof(text)){
        n=read(fd,text+used,sizeof(text)-used);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)break;used+=(size_t)n;
    }
    close(fd);
    return n>=0 && used<sizeof(text) && !boot_profile_parse(text,used,out) ? 0 : fail("invalid profile");
}

/* SHA-256, streaming full-image digest. All arithmetic is unsigned 32-bit. */
static const uint32_t k[64]={
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
static uint32_t ror(uint32_t v,unsigned n) { return v>>n | v<<(32-n); }
static void block(uint32_t h[8],const unsigned char b[64]) {
    uint32_t w[64];
    for (unsigned i=0;i<16;i++) w[i]=(uint32_t)b[4*i]<<24|(uint32_t)b[4*i+1]<<16|(uint32_t)b[4*i+2]<<8|b[4*i+3];
    for (unsigned i=16;i<64;i++) w[i]=w[i-16]+(ror(w[i-15],7)^ror(w[i-15],18)^(w[i-15]>>3))+w[i-7]+(ror(w[i-2],17)^ror(w[i-2],19)^(w[i-2]>>10));
    uint32_t a=h[0],b_=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for (unsigned i=0;i<64;i++) {
        uint32_t t=hh+(ror(e,6)^ror(e,11)^ror(e,25))+((e&f)^(~e&g))+k[i]+w[i];
        uint32_t u=(ror(a,2)^ror(a,13)^ror(a,22))+((a&b_)^(a&c)^(b_&c));
        hh=g;g=f;f=e;e=d+t;d=c;c=b_;b_=a;a=t+u;
    }
    h[0]+=a;h[1]+=b_;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
}
int boot_sha256_fd(int fd,uint64_t size,char hex[65]) {
    uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    unsigned char b[64]; uint64_t done=0;
    while (size-done>=64) {
        size_t used=0;
        while (used<64) { ssize_t n=read(fd,b+used,64-used); if(n<=0)return -1; used+=(size_t)n; }
        block(h,b);done+=64;
    }
    size_t left=(size_t)(size-done),used=0;
    while(used<left) { ssize_t n=read(fd,b+used,left-used);if(n<=0)return -1;used+=(size_t)n; }
    memset(b+left,0,64-left);b[left]=0x80;
    if(left>=56){block(h,b);memset(b,0,64);}
    uint64_t bits=size*8;
    for(unsigned i=0;i<8;i++)b[63-i]=(unsigned char)(bits>>(8*i));
    block(h,b);
    static const char digits[]="0123456789abcdef";
    for(unsigned i=0;i<32;i++){unsigned byte=(h[i/4]>>(24-8*(i%4)))&255;hex[i*2]=digits[byte>>4];hex[i*2+1]=digits[byte&15];}
    hex[64]=0;return 0;
}

int boot_profile_identity(const struct boot_profile *p) {
    struct utsname un;char text[4096],hash[65];
    if(uname(&un)||strcmp(un.release,p->release)||strcmp(un.version,p->version))return fail("kernel identity mismatch");
#ifndef NATARTOS_BOOT_TEST
    if(strcmp(un.machine,"aarch64"))return fail("machine mismatch");
#endif
    int fd=open(SLOT,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return fail("boot slot unavailable");
    ssize_t n=read(fd,text,sizeof(text)-1);close(fd);if(n<=0||n==(ssize_t)sizeof(text)-1)return fail("invalid command line");text[n]=0;
    /* Consume only this token; never log the private command line. */
    unsigned slot=0,seen=0;char *save=NULL;
    for(char *t=strtok_r(text," \n",&save);t;t=strtok_r(NULL," \n",&save))
        if(!strncmp(t,"androidboot.slot_suffix=",24)){seen++;if(!strcmp(t+24,"1"))slot=1;else if(!strcmp(t+24,"2"))slot=2;}
    if(seen!=1||slot!=p->slot)return fail("boot slot mismatch");
    fd=open(slot==1?IMAGE1:IMAGE2,O_RDONLY|O_CLOEXEC);if(fd<0)return fail("boot image unavailable");
    struct stat st;uint64_t size=0;
    if(fstat(fd,&st)){close(fd);return -1;}
#ifdef NATARTOS_BOOT_TEST
    if(S_ISREG(st.st_mode))size=(uint64_t)st.st_size;
#else
    if(S_ISBLK(st.st_mode)&&ioctl(fd,0x80081272,&size)==0&&size==33554432){}else{close(fd);return fail("unexpected boot device/size");}
#endif
    int rc=size<=33554432?boot_sha256_fd(fd,size,hash):-1;close(fd);
    if(rc||strcmp(hash,p->image_sha256))return fail("boot image digest mismatch");
    return 0;
}

static int enforcement(void) {
    char b[8];int fd=open(ENFORCE,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -1;
    ssize_t n=read(fd,b,sizeof(b));close(fd);
    return n>0&&(b[0]=='0'||b[0]=='1')&&(n==1||(n==2&&b[1]=='\n'))?b[0]-'0':-1;
}
static int memory_open(int flags) {
    int fd=open(MEMORY,flags|O_CLOEXEC|O_NOFOLLOW);struct stat st;
    if(fd<0)return -1;
    if(fstat(fd,&st)){close(fd);return -1;}
#ifdef NATARTOS_BOOT_TEST
    int ok=S_ISREG(st.st_mode);
#else
    int ok=S_ISCHR(st.st_mode)&&major(st.st_rdev)==1&&minor(st.st_rdev)==1;
#endif
    if(!ok){close(fd);errno=EPERM;return -1;}return fd;
}
int boot_permission_inspect(uint64_t physical,struct permission_check *out) {
    if(physical<0x80000||physical>0x3b9ffff8||(physical&7))return fail("invalid RAM address");
    out->enforcing=enforcement();if(out->enforcing<0)return fail("cannot read enforcement");
    int fd=memory_open(O_RDONLY);if(fd<0)return fail("cannot open physical memory read-only");
    ssize_t n=pread(fd,&out->word,4,(off_t)physical);close(fd);
    if(n!=4)return fail("short physical-memory read");
    return 0;
}
int boot_permission_check(const struct boot_profile *p,struct permission_check *out) {
    if(boot_profile_identity(p)||boot_permission_inspect(p->state_phys,out))return -1;
    if(out->word!=(out->enforcing?0x01000100U:0x01000000U))return fail("SELinux state signature mismatch");
    return 0;
}
#ifndef NATARTOS_BOOT_READ_ONLY
int boot_permission_apply(const struct boot_profile *p,int (*cancelled)(void)) {
    struct permission_check check;
    /* Rehash and reread immediately before opening a writable descriptor. */
    if(boot_permission_check(p,&check))return -1;
    if(cancelled&&cancelled())return fail("permission transition bypassed");
#ifndef NATARTOS_BOOT_TEST
    if(!cancelled)return fail("permission transition requires bypass check");
#endif
    if(!check.enforcing)return 0;
    int fd=memory_open(O_RDWR);if(fd<0)return fail("cannot open permission byte");
    uint32_t word=0;
    if(pread(fd,&word,4,(off_t)p->state_phys)!=4||word!=0x01000100U){close(fd);return fail("state changed before write");}
    if(cancelled&&cancelled()){close(fd);return fail("permission transition bypassed");}
    unsigned char zero=0;
    ssize_t n=pwrite(fd,&zero,1,(off_t)p->state_phys+1);close(fd);
    if(n!=1)return fail("permission-byte write failed");
    if(boot_permission_check(p,&check)||check.enforcing||check.word!=0x01000000U)return fail("permission transition not confirmed");
    return 0;
}
#endif
