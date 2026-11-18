/* SPDX-License-Identifier: AGPL-3.0-only */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc,char **argv){
    if(argc!=2||strcmp(argv[1],"--boot")||getenv("LD_PRELOAD")||getenv("TRIGGER_SECRET"))return 1;
    int fd=open("/trigger/result",O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return 1;
    if(write(fd,"ok",2)!=2)return 1;close(fd);sleep(2);return 0;
}
