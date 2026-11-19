/*
 * mod_sd_apps.so — More-menu entries for apps on the SD card.
 *
 * While dji_glasses starts, asks the standalone runner which packages the
 * card holds (`sd_app_runner menu-list`) and registers one menu entry per
 * app. Tapping an entry starts `sd_app_runner run <id>` in the background:
 * the runner verifies and stages the package on internal storage, gives
 * "display": true apps the screen through the overlay, and hands it back
 * when they exit. All package parsing stays in the runner, outside
 * dji_glasses.
 *
 * The card is read once per dji_glasses start (natartos start or a respawn);
 * apps copied afterwards appear after the next start.
 */

#define NTLOG_PREFIX "sd_apps"
#include "natartos_common.h"

#ifndef X_OK
#define X_OK 1
#endif
#define MAX_APPS 24          /* of NATARTOS_MAX_MENU_ITEMS; leaves room for other mods */
#define PRIORITY_BASE 300    /* after Doom (100) and PC Stream (200) */
#define ID_MAX 48            /* APP_ID_MAX in mods/sd_apps/package.h */

/* Installed location first; the /data copy serves bench builds until the
 * installer ships the runner. Both directories are writable only by root.
 * (Host tests substitute their own runner; see tests/sd_apps/mod_check.c.) */
#ifndef SD_APPS_RUNNERS
#define SD_APPS_RUNNERS "/vendor/natartos/apps/sd_apps/sd_app_runner", \
                        "/data/natartos/bin/sd_app_runner"
#endif
static const char *const RUNNERS[] = { SD_APPS_RUNNERS };
#define ERR_LOG " 2>>" NATARTOS_LOG_PATH

typedef void *(*popen_fn)(const char *, const char *);
typedef char *(*fgets_fn)(char *, int, void *);
typedef int (*pclose_fn)(void *);

/* Bounded concatenation: 0 on success, -1 if `cap` would be exceeded. */
static int append(char *dst, int *len, int cap, const char *src) {
    int n = natartos_strlen(src);
    if (*len + n >= cap) return -1;
    natartos_memcpy(dst + *len, src, n);
    *len += n;
    dst[*len] = 0;
    return 0;
}

/* Same rule as the runner's app_id_valid: [a-z0-9_-], starting alphanumeric.
 * The id is placed in a shell command, so never trust the listing blindly. */
static int id_valid(const char *s, int n) {
    if (n < 1 || n > ID_MAX) return 0;
    if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= '0' && s[0] <= '9'))) return 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}

/* Split "id\tversion\tname\n" in place. 0 on success. */
static int parse_line(char *line, char **id, char **version, char **name) {
    int n = natartos_strlen(line);
    if (n == 0 || line[n - 1] != '\n') return -1;   /* incomplete (listing cut short) */
    line[--n] = 0;
    char *field[3];
    int count = 0;
    field[count++] = line;
    for (int i = 0; i < n; i++) {
        if (line[i] == '\t') {
            if (count == 3) return -1;
            line[i] = 0;
            field[count++] = line + i + 1;
        }
    }
    if (count != 3 || !field[2][0]) return -1;
    *id = field[0]; *version = field[1]; *name = field[2];
    return 0;
}

__attribute__((constructor))
static void init_hook(void) {
    char exe[64];
    int fd = (int)sys_openat(AT_FDCWD, "/proc/self/cmdline", O_RDONLY, 0);
    if (fd >= 0) {
        long r = sys_read(fd, exe, sizeof(exe) - 1);
        sys_close(fd);
        if (r > 0) {
            exe[r] = 0;
            for (int i = 0; i + 11 <= r; i++)
                if (natartos_strncmp(exe + i, "dji_glasses", 11) == 0) {
                    NTLOG("mod_sd_apps loaded");
                    return;
                }
        }
    }
}

int natartos_mod_init(void *(*loader_dlsym)(void *, const char *)) {
    (void)loader_dlsym;
    return 0;
}

int natartos_mod_register(void *(*loader_dlsym)(void *, const char *)) {
    natartos_menu_register_fn reg =
        (natartos_menu_register_fn)loader_dlsym(RTLD_DEFAULT, "natartos_menu_register");
    if (!reg) { NTLOG("menu_register not found (menu mod not enabled?)"); return 0; }
    popen_fn p_open = (popen_fn)loader_dlsym(RTLD_DEFAULT, "popen");
    fgets_fn p_gets = (fgets_fn)loader_dlsym(RTLD_DEFAULT, "fgets");
    pclose_fn p_close = (pclose_fn)loader_dlsym(RTLD_DEFAULT, "pclose");
    if (!p_open || !p_gets || !p_close) { NTLOG("popen/fgets/pclose not found"); return 0; }

    const char *runner = 0;
    for (unsigned i = 0; i < sizeof(RUNNERS) / sizeof(RUNNERS[0]); i++)
        if (sys_faccessat(AT_FDCWD, RUNNERS[i], X_OK) == 0) { runner = RUNNERS[i]; break; }
    if (!runner) { NTLOG("sd_app_runner not installed; no SD app entries"); return 0; }
    NTLOG(runner);

    /* The runner discovers the card itself and gives up after 5 s. */
    char list_cmd[192];
    int len = 0;
    list_cmd[0] = 0;
    if (append(list_cmd, &len, sizeof(list_cmd), runner) ||
        append(list_cmd, &len, sizeof(list_cmd), " menu-list" ERR_LOG)) return 0;
    void *listing = p_open(list_cmd, "r");
    if (!listing) { NTLOG("menu-list: popen failed"); return 0; }

    char line[512];
    int added = 0;
    while (p_gets(line, sizeof(line), listing)) {
        char *id, *version, *name;
        if (added >= MAX_APPS) continue;   /* drain the rest */
        if (parse_line(line, &id, &version, &name) || !id_valid(id, natartos_strlen(id))) {
            NTLOG("menu-list: unexpected line skipped");
            continue;
        }
        /* "sd:" keeps card apps from replacing another mod's entry (e.g. "doom"). */
        char key[8 + ID_MAX], subtitle[64], command[256];
        int k = 0, s = 0, c = 0;
        key[0] = subtitle[0] = command[0] = 0;
        if (append(key, &k, sizeof(key), "sd:") || append(key, &k, sizeof(key), id)) continue;
        append(subtitle, &s, sizeof(subtitle), "SD card app ");
        append(subtitle, &s, sizeof(subtitle), version);   /* left out if too long */
        if (append(command, &c, sizeof(command), "nohup ") ||
            append(command, &c, sizeof(command), runner) ||
            append(command, &c, sizeof(command), " run ") ||
            append(command, &c, sizeof(command), id) ||
            append(command, &c, sizeof(command), " >/dev/null" ERR_LOG " &")) continue;
        natartos_menu_entry entry = {
            .id = key,
            .name = name,
            .subtitle = subtitle,
            .icon_path = 0,
            .command = command,
            .priority = PRIORITY_BASE + added,
        };
        if (reg(&entry) != 0) { NTLOG("menu full; remaining SD apps not shown"); added = MAX_APPS; continue; }
        added++;
    }
    int status = p_close(listing);
    NTLOG_INT("SD app entries: ", added);
    if (status != 0) NTLOG_INT("menu-list wait status: ", status);
    return 1;
}
