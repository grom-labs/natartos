/* SPDX-License-Identifier: AGPL-3.0-only */
#define _GNU_SOURCE
#undef NDEBUG
#include "profile.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

static void put(const char *p,const void *b,size_t n) {
    int fd=open(p,O_WRONLY|O_CREAT|O_TRUNC,0600);assert(fd>=0);assert(write(fd,b,n)==(ssize_t)n);close(fd);
}
static void digest(const char *text,const char *expected) {
    put("/fixture/hash",text,strlen(text));char hex[65];int fd=open("/fixture/hash",O_RDONLY);assert(fd>=0);
    assert(!boot_sha256_fd(fd,strlen(text),hex));close(fd);assert(!strcmp(hex,expected));
}
static int cancelled(void){return 1;}
int main(void) {
    assert(!mkdir("/fixture",0700));
    digest("","e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    digest("abc","ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    digest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq","248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    char million[1000000];memset(million,'a',sizeof(million));put("/fixture/hash",million,sizeof(million));
    int fd=open("/fixture/hash",O_RDONLY);char hex[65];assert(!boot_sha256_fd(fd,sizeof(million),hex));close(fd);
    assert(!strcmp(hex,"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    fd=open("/fixture/hash",O_RDONLY);assert(boot_sha256_fd(fd,sizeof(million)+1,hex));close(fd);
    struct utsname un;assert(!uname(&un));char text[1024];
    snprintf(text,sizeof(text),"schema=1\nrelease=%s\nversion=%s\nslot=1\nimage_sha256=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\nstate_phys=0x80000\n",un.release,un.version);
    struct boot_profile p;assert(!boot_profile_parse(text,strlen(text),&p));
    assert(boot_profile_parse(text,strlen(text)-1,&p));
    char corrupt[1100];snprintf(corrupt,sizeof(corrupt),"%sslot=1\n",text);assert(boot_profile_parse(corrupt,strlen(corrupt),&p));
    snprintf(corrupt,sizeof(corrupt),"%sunknown=1\n",text);assert(boot_profile_parse(corrupt,strlen(corrupt),&p));
    const char *bad[]={"0x0","0x80001","0x40000000","0X80000","0x80000junk","-1"};
    char *at=strstr(text,"state_phys=");assert(at);size_t prefix=(size_t)(at-text)+11;
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++){
        memcpy(corrupt,text,prefix);snprintf(corrupt+prefix,sizeof(corrupt)-prefix,"%s\n",bad[i]);assert(boot_profile_parse(corrupt,strlen(corrupt),&p));
    }
    assert(!boot_profile_parse(text,strlen(text),&p));
    put("/fixture/profile",text,strlen(text));assert(!boot_profile_load("/fixture/profile",&p));
    assert(!chmod("/fixture/profile",0666));assert(boot_profile_load("/fixture/profile",&p));
    assert(!chmod("/fixture/profile",0600));assert(!symlink("/fixture/profile","/fixture/link"));assert(boot_profile_load("/fixture/link",&p));
    put("/fixture/normal","abc",3);put("/fixture/slot","androidboot.slot_suffix=1 private=secret\n",strlen("androidboot.slot_suffix=1 private=secret\n"));
    assert(!boot_profile_parse(text,strlen(text),&p));assert(!boot_profile_identity(&p));
    p.slot=2;assert(boot_profile_identity(&p));p.slot=1;
    p.release[0]='?';assert(boot_profile_identity(&p));assert(!boot_profile_parse(text,strlen(text),&p));
    put("/fixture/normal","abd",3);assert(boot_profile_identity(&p));put("/fixture/normal","abc",3);
    put("/fixture/slot","androidboot.slot_suffix=1 androidboot.slot_suffix=1",strlen("androidboot.slot_suffix=1 androidboot.slot_suffix=1"));assert(boot_profile_identity(&p));
    put("/fixture/slot","androidboot.slot_suffix=1\n",strlen("androidboot.slot_suffix=1\n"));
    fd=open("/fixture/memory",O_CREAT|O_RDWR,0600);assert(fd>=0);assert(!ftruncate(fd,0x80008));
    uint32_t word=0x01000100;assert(pwrite(fd,&word,4,0x80000)==4);close(fd);
    put("/fixture/enforce","1",1);struct permission_check c;assert(!boot_permission_check(&p,&c));assert(c.word==word&&c.enforcing==1);
    put("/fixture/enforce","0",1);assert(boot_permission_check(&p,&c));
    word=0x01000000;fd=open("/fixture/memory",O_RDWR);assert(pwrite(fd,&word,4,0x80000)==4);close(fd);
    assert(!boot_permission_check(&p,&c));assert(!boot_permission_apply(&p,NULL));
    /* A bypass prevents opening for a write; the simulated kernel then proves
     * the apply path changes only the one permission byte. Its sysfs fixture
     * does not follow RAM, so the post-write verification must report failure. */
    unsigned char bytes[8]={0,1,0,1,0xaa,0xbb,0xcc,0xdd},actual[8];
    fd=open("/fixture/memory",O_RDWR);assert(pwrite(fd,bytes,8,0x80000)==8);close(fd);put("/fixture/enforce","1",1);
    assert(boot_permission_apply(&p,cancelled));
    fd=open("/fixture/memory",O_RDONLY);assert(pread(fd,actual,8,0x80000)==8);close(fd);assert(!memcmp(bytes,actual,8));
    assert(boot_permission_apply(&p,NULL));bytes[1]=0;
    fd=open("/fixture/memory",O_RDONLY);assert(pread(fd,actual,8,0x80000)==8);close(fd);assert(!memcmp(bytes,actual,8));
    put("/fixture/enforce","0",1);assert(!boot_permission_check(&p,&c));
    word=0xdeadbeef;fd=open("/fixture/memory",O_RDWR);assert(pwrite(fd,&word,4,0x80000)==4);close(fd);
    assert(boot_permission_apply(&p,NULL));fd=open("/fixture/memory",O_RDONLY);uint32_t after=0;assert(pread(fd,&after,4,0x80000)==4);close(fd);assert(after==word);
    put("/fixture/enforce","invalid",7);assert(boot_permission_check(&p,&c));
    puts("BOOT_PROFILE_TESTS_PASSED");return 0;
}
