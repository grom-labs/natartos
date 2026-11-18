/* SPDX-License-Identifier: AGPL-3.0-only */
#ifndef NATARTOS_BOOT_PROFILE_H
#define NATARTOS_BOOT_PROFILE_H
#include <stddef.h>
#include <stdint.h>

struct boot_profile {
    char release[128], version[192], image_sha256[65];
    unsigned slot;
    uint64_t state_phys;
};
struct permission_check {
    uint32_t word;
    int enforcing;
};

int boot_profile_parse(const char *text, size_t size, struct boot_profile *out);
void boot_profile_set_logger(void (*logger)(const char *));
int boot_profile_load(const char *path, struct boot_profile *out);
int boot_profile_identity(const struct boot_profile *profile);
int boot_permission_check(const struct boot_profile *profile, struct permission_check *out);
int boot_permission_apply(const struct boot_profile *profile, int (*cancelled)(void));
int boot_permission_inspect(uint64_t physical, struct permission_check *out);
int boot_sha256_fd(int fd, uint64_t size, char hex[65]);
int boot_trusted_file(const char *path, int flags);
#endif
