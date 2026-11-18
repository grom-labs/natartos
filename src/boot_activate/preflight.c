/* SPDX-License-Identifier: AGPL-3.0-only */
#include "profile.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(int argc,char **argv) {
    if(argc!=3||strcmp(argv[1],"--preflight")){fprintf(stderr,"usage: boot-preflight --preflight PROFILE\n");return 2;}
    if(geteuid()){fprintf(stderr,"boot: root required\n");return 1;}
    struct boot_profile p;struct permission_check c;
    if(boot_profile_load(argv[2],&p)||boot_permission_check(&p,&c))return 1;
    printf("BOOT_PREFLIGHT_OK schema=1 word=0x%08" PRIx32 " enforcing=%d writes=0\n",c.word,c.enforcing);
    return 0;
}
