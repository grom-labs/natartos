/* SPDX-License-Identifier: AGPL-3.0-only
 * LINK-TIME INTERFACE ONLY. Never install this file's compiled libdl.so.
 * Its SONAME records the Android runtime dependency without vendor binaries
 * or glibc symbol versions. Trap bodies make accidental execution explicit.
 */
void *dlopen(const char *path, int flags) { (void)path; (void)flags; __builtin_trap(); }
void *dlsym(void *handle, const char *name) { (void)handle; (void)name; __builtin_trap(); }
