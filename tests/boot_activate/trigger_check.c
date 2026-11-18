/* SPDX-License-Identifier: AGPL-3.0-only */
#define _GNU_SOURCE
#undef NDEBUG
#include "trigger.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static void text(const char *p,const char *s){int fd=open(p,O_WRONLY|O_CREAT|O_TRUNC,0600);assert(fd>=0);size_t n=0;while(s[n])n++;assert(write(fd,s,n)==(ssize_t)n);close(fd);}
static int exists(void){return !access("/trigger/result",F_OK);}
static void skipped(void){boot_activation_start();usleep(50000);assert(!exists());}
int main(int argc,char **argv){
    assert(argc==2);alarm(8);assert(!mkdir("/trigger",0700));
    int in=open(argv[1],O_RDONLY),out=open("/trigger/helper",O_WRONLY|O_CREAT,0700);assert(in>=0&&out>=0);char b[4096];ssize_t n;
    while((n=read(in,b,sizeof(b)))>0)assert(write(out,b,(size_t)n)==n);assert(n==0);close(in);close(out);
    skipped();
    text("/trigger/enabled","invalid");skipped();
    text("/trigger/enabled","natartos-boot-v1\n");text("/trigger/disabled","");skipped();unlink("/trigger/disabled");
    assert(!chmod("/trigger/helper",0777));skipped();assert(!chmod("/trigger/helper",0700));
    assert(!chmod("/trigger/enabled",0666));skipped();assert(!chmod("/trigger/enabled",0600));
    assert(!rename("/trigger/enabled","/trigger/real-enabled"));assert(!symlink("/trigger/real-enabled","/trigger/enabled"));skipped();assert(!unlink("/trigger/enabled"));assert(!rename("/trigger/real-enabled","/trigger/enabled"));
    setenv("TRIGGER_SECRET","must-not-inherit",1);
    struct timespec start,end;clock_gettime(CLOCK_MONOTONIC,&start);boot_activation_start();clock_gettime(CLOCK_MONOTONIC,&end);
    assert((end.tv_sec-start.tv_sec)*1000000000L+end.tv_nsec-start.tv_nsec<1000000000L);
    for(unsigned i=0;i<100&&!exists();i++)usleep(10000);assert(exists());
    puts("BOOT_TRIGGER_TESTS_PASSED=7");return 0;
}
