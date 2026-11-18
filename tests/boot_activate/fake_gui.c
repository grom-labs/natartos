/* SPDX-License-Identifier: AGPL-3.0-only */
#define _GNU_SOURCE
#include "loader_health.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
int main(int argc,char **argv){
    if(argc!=3||strcmp(argv[1],"-g")||strcmp(argv[2],"-f"))return 1;
    FILE *f=fopen("/fixture/starts","a");if(!f)return 1;fprintf(f,"%ld\n",(long)getpid());fclose(f);
    char mode[32]={0};f=fopen("/fixture/mode","r");if(!f)return 1;fgets(mode,sizeof(mode),f);fclose(f);
    if(!strcmp(mode,"crash"))return 1;
    int health=boot_health_setup(NULL);if(health!=1)return 2;
    if(!strcmp(mode,"healthy")||!strcmp(mode,"ignoreterm"))boot_health_finish(1);
    else if(!strcmp(mode,"badnonce")){boot_health_nonce++;boot_health_finish(1);}
    else if(!strcmp(mode,"eof"))boot_health_finish(0);
    if(!strcmp(mode,"ignoreterm"))signal(SIGTERM,SIG_IGN);
    for(;;)pause();
}
