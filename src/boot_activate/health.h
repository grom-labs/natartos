/* SPDX-License-Identifier: AGPL-3.0-only */
#ifndef NATARTOS_BOOT_HEALTH_H
#define NATARTOS_BOOT_HEALTH_H
#include <stdint.h>
struct boot_health_message { char magic[8]; uint64_t nonce; uint32_t pid, rendered; };
_Static_assert(sizeof(struct boot_health_message)==24,"boot health wire ABI");
#define BOOT_HEALTH_MAGIC "NATBOOT1"
#endif
