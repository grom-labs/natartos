#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <linux/input.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    if (argc >= 3 && !strcmp(argv[1], "unprivileged")) {
        if (geteuid() == 0 && (setgid(65534) || setuid(65534))) { perror("drop test privileges"); return 1; }
        execv(argv[2], argv + 2);
        perror("execute test runner without privileges");
        return 1;
    }
    if (argc == 4 && !strcmp(argv[1], "key")) {
        /* One evdev key event plus SYN, as a button device would report it. */
        struct input_event ev[2];
        memset(ev, 0, sizeof(ev));
        ev[0].type = EV_KEY;
        ev[0].code = (unsigned short)atoi(argv[2]);
        ev[0].value = atoi(argv[3]);
        return write(STDOUT_FILENO, ev, sizeof(ev)) == (ssize_t)sizeof(ev) ? 0 : 1;
    }
    if (!strcmp(argv[1], "fds")) {
        /* Only stdin/stdout/stderr may reach an app. */
        DIR *d = opendir("/proc/self/fd");
        if (!d) return 1;
        struct dirent *e;
        int leaked = 0;
        while ((e = readdir(d))) {
            int fd = atoi(e->d_name);
            if (e->d_name[0] != '.' && fd > 2 && fd != dirfd(d)) {
                fprintf(stderr, "inherited fd %d\n", fd);
                leaked = 1;
            }
        }
        closedir(d);
        return leaked ? 9 : 0;
    }
    char ownpath[4096];
    snprintf(ownpath, sizeof(ownpath), "%s/self.pid", getenv("NATARTOS_APP_DATA"));
    FILE *own = fopen(ownpath, "w");
    if (!own) return 1;
    fprintf(own, "%ld\n", (long)getpid());
    fclose(own);
    if (!strcmp(argv[1], "exit7")) return 7;
    if (!strcmp(argv[1], "crash")) { raise(SIGSEGV); return 1; }
    if (!strcmp(argv[1], "flood")) {
        char block[4096]; memset(block, 'x', sizeof(block));
        for (unsigned i=0; i<1024; ++i) if (fwrite(block, 1, sizeof(block), stdout) != sizeof(block)) return 1;
        return 0;
    }
    if (!strcmp(argv[1], "child")) {
        pid_t pid = fork();
        if (pid < 0) return 1;
        if (!pid) { for (;;) pause(); }
        char path[4096];
        snprintf(path, sizeof(path), "%s/child.pid", getenv("NATARTOS_APP_DATA"));
        FILE *file = fopen(path, "w");
        if (!file) return 1;
        fprintf(file, "%ld\n", (long)pid);
        fclose(file);
        return 0;
    }
    if (!strcmp(argv[1], "ignoreterm")) signal(SIGTERM, SIG_IGN);
    if (!strcmp(argv[1], "closeout")) { close(1); close(2); }
    for (;;) pause();
}
