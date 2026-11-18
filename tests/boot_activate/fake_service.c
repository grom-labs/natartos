/* SPDX-License-Identifier: AGPL-3.0-only */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(int argc,char **argv){
    const char *path="/fixture/service";
    if(strstr(argv[0],"setprop")){
        if(argc!=3||strcmp(argv[1],"dji.glasses_service")||(strcmp(argv[2],"0")&&strcmp(argv[2],"1")))return 1;
        FILE *f=fopen(path,"w");if(!f)return 1;fputs(!strcmp(argv[2],"0")?"stopped\n":"running\n",f);return fclose(f)?1:0;
    }
    if(argc!=2||strcmp(argv[1],"init.svc.dji_glasses"))return 1;
    if(!access("/fixture/flood",F_OK)){char b[256];memset(b,'x',sizeof(b));for(;;)if(write(1,b,sizeof(b))<0)return 0;}
    char b[64];FILE *f=fopen(path,"r");if(!f)return 1;if(!fgets(b,sizeof(b),f)){fclose(f);return 1;}fclose(f);fputs(b,stdout);return 0;
}
