/* SPDX-License-Identifier: AGPL-3.0-only — authored fixture, no vendor code. */
#include <stdint.h>
static int init(void *argument) { return argument == (void *)(uintptr_t)0x1234 ? 17 : 19; }
static int start(void) { return 23; }
struct interface { unsigned long size; int (*init)(void *); int (*start)(void); };
static const struct interface first = {sizeof(struct interface), init, start};
static const struct interface second = {sizeof(struct interface), init, start};
#ifndef STOCK_MISSING_SYMBOL
const void *gps_get_hardware_interface(void *argument) {
    if (argument == (void *)(uintptr_t)0xDEAD) return 0;
    return argument == (void *)(uintptr_t)0x2345 ? &second : &first;
}
#else
const void *unrelated_symbol(void *argument) { (void)argument; return &first == &second ? 0 : &first; }
#endif
