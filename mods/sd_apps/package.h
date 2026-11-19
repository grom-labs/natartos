#ifndef SD_APPS_PACKAGE_H
#define SD_APPS_PACKAGE_H

#include <stddef.h>

#define APP_ID_MAX 48
#define APP_TEXT_MAX 128
#define APP_PATH_MAX 512
#define APP_ARGS_MAX 16
#define APP_ARG_MAX 256
#define APP_MANIFEST_MAX 8192

typedef struct {
    char id[APP_ID_MAX + 1];
    char name[APP_TEXT_MAX + 1];
    char version[65];
    char entry[APP_PATH_MAX];
    char args[APP_ARGS_MAX][APP_ARG_MAX];
    unsigned argc;
    int display;  /* draws through the natartos overlay (optional, default false) */
} app_manifest;

int app_id_valid(const char *id);
int app_path_valid(const char *path);
int app_manifest_parse(const char *text, size_t length, app_manifest *out,
                       char *error, size_t error_size);
/* Static ELF only in this first slice; no Android linker/libc dependency. */
int app_elf_validate(int fd, char *error, size_t error_size);

#endif
