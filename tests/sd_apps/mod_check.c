/*
 * Host check for mods/sd_apps/mod.c. Compiles the mod against
 * tests/sd_apps/shim/natartos_common.h, gives it real popen/fgets/pclose and
 * a recording natartos_menu_register, then prints what it registered:
 *
 *   key<TAB>name<TAB>subtitle<TAB>priority<TAB>command
 *
 * The mod looks for ./sd-runner (relative to the working directory), which
 * the test script points at the runner under test.
 */
#define _POSIX_C_SOURCE 200809L
#define SD_APPS_RUNNERS "./sd-runner"
#include "../../mods/sd_apps/mod.c"

#include <stdlib.h>

static int registered;
static int full_after = -1;   /* simulate a full menu after N entries */

static int record(const natartos_menu_entry *e) {
    if (full_after >= 0 && registered >= full_after) return -2;
    ++registered;
    printf("%s\t%s\t%s\t%d\t%s\n", e->id, e->name, e->subtitle ? e->subtitle : "",
           e->priority, e->command ? e->command : "");
    return 0;
}

static void *fake_dlsym(void *handle, const char *name) {
    (void)handle;
    if (!strcmp(name, "natartos_menu_register")) return (void *)record;
    if (!strcmp(name, "popen")) return (void *)popen;
    if (!strcmp(name, "fgets")) return (void *)fgets;
    if (!strcmp(name, "pclose")) return (void *)pclose;
    return NULL;
}

int main(int argc, char **argv) {
    if (argc > 1) full_after = atoi(argv[1]);
    natartos_mod_init(fake_dlsym);
    natartos_mod_register(fake_dlsym);
    return 0;
}
