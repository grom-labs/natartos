#include "../../mods/sd_apps/sha256.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    const char *expected[] = {
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"
    };
    for (unsigned test=0; test<3; ++test) {
        app_sha256 hash;
        char hex[65];
        app_sha256_init(&hash);
        if (test == 1) app_sha256_update(&hash, "abc", 3);
        if (test == 2) for (unsigned i=0; i<1000000; ++i) app_sha256_update(&hash, "a", 1);
        app_sha256_final(&hash, hex);
        if (strcmp(hex, expected[test])) { fprintf(stderr, "SHA-256 vector %u failed\n", test); return 1; }
    }
    puts("SHA-256 known vectors passed");
    return 0;
}
