#ifndef SD_APPS_SHA256_H
#define SD_APPS_SHA256_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
    uint32_t state[8];
    uint64_t bytes;
    unsigned used;
    unsigned char block[64];
} app_sha256;
void app_sha256_init(app_sha256 *s);
void app_sha256_update(app_sha256 *s, const void *data, size_t length);
void app_sha256_final(app_sha256 *s, char hex[65]);
#endif
