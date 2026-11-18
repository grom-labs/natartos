# Experimental boot marker probe

This standalone prototype tests a possible native boot execution point. It
forwards `gps_get_hardware_interface` to a separately preserved stock library
and appends diagnostics to an existing marker file. It does not activate
Natartos, enable ADB, change SELinux, access physical memory, launch processes
or modify GPS callbacks. The normal release and installer do not include it.

**Status:** built and tested with authored fixtures on Linux ARM64 and x86-64;
the production library also passed an isolated Android-linker smoke test. The
[device bench](BOOT_MARKER_BENCH_2026-11-17.md) produced a fresh marker after a
full power cycle in `dji_services` with **SELinux enforcing**. The original GPS
library was restored afterward. Automatic Natartos activation, a satellite
position fix and flight operation remain unvalidated.

## ABI and paths

The supported entry is the getter-based interface:

```c
const void *gps_get_hardware_interface(void *device);
```

The constructor loads `/vendor/lib64/gps.default.stock.so` with
`RTLD_NOW | RTLD_LOCAL` and resolves that same getter. Every subsequent call
passes its argument unchanged and returns the original pointer unchanged,
including a null result. The original handle is retained for the process
lifetime so returned callbacks remain mapped. Initialization finishes before
the loading `dlopen` returns; getter reads use acquire/release pointer access.

This supports the observed getter consumer. It does **not** reproduce every
export of the original library or provide the `HMI` HAL descriptor. Consumers
requiring other exports are outside this prototype's compatibility contract.

An unavailable original, missing getter or self-reference leaves forwarding
unavailable and returns null. **That can break or crash a consumer that assumes
a non-null interface.** Detecting these cases in tests does not make an invalid
installation safe. There is no automatic restoration or tested device recovery.

## Marker behavior

The production marker is `/data/dji/natartos_boot_probe.log`. It must already
be a regular file owned by the process's effective uid, without group/world
write permission. For a root service, prepare it as root with mode `0600` in
a trusted directory. The proxy never creates, truncates or changes its mode.

One record is attempted per proxy initialization:

```text
natartos_boot_probe v=1 phase=load pid=... uid=... euid=... gid=... egid=... stock=ready context=... CapEff=... enforcing=... boot_id=...
```

Only the SELinux context, effective capability field, enforcement status and
boot ID are read from `/proc` or `/sys`. Unavailable diagnostics are recorded
as `unreadable:<negative syscall error>` (or the read length when empty/missing
from a readable status file). No environment, unit identifiers or memory contents
are collected. A record proves constructor execution and symbol resolution;
it does not prove a cold boot, successful GPS initialization or mod readiness.
The boot ID helps distinguish records from different boots.

Marker opens use nonblocking mode and reject a final symlink. A nonblocking
file lock serializes cooperating probes, and the file is checked again before
writing. Missing/unsuitable markers, held locks and unavailable `statx` are
skipped while forwarding stays available. Only complete records fitting within
64 KiB are attempted. There is one write, no retry loop and no `fsync`; an I/O
failure or interrupted write can leave no record or a partial one. Ordinary
filesystem I/O and stock library initialization do not have a wall-clock timeout.
Ancestor directories must be trusted; final-symlink rejection is not a sandbox.

Removing the marker prevents diagnostic appends. It does not uninstall or
bypass the forwarding proxy.

## Build without device access

With an ARM64 Linux cross compiler and Python 3:

```sh
python3 tools/boot_probe/build.py --arch aarch64
```

Alternatively, use a local Zig **0.15.2** executable on Windows or Linux:

```sh
python tools/boot_probe/build.py --zig /path/to/zig --arch aarch64 --tests
```

The candidate is `build/boot-probe/aarch64/gps.default.so`. Its adjacent
`build-info.json` records source/artifact hashes, target and ELF audit results.
The audit requires one constructor, the single exported getter, just the two
`dlopen`/`dlsym` imports, a `libdl.so` dependency and a non-executable stack.
No vendor binaries are needed to build it. The Zig musl target supplies compiler
headers only; the production proxy is freestanding and does not depend on musl.

`build/boot-probe/aarch64/link-only/libdl.so` is an **authored linker interface**,
not a runtime library. Its functions trap if executed. It records the Android
`libdl.so` dependency without embedding vendor code or glibc symbol versions.
**Never install that interface or copy the build directory as a device tree.**
The `tests/` outputs have different paths/linkage for Linux fixtures and are
also excluded from deployment.

## Fixture tests

On Linux with a native x86-64 compiler:

```sh
python3 tools/boot_probe/build.py --arch x86_64 --tests
build/boot-probe/x86_64/tests/check build/boot-probe/x86_64/tests/gps.default.so build/boot-probe/x86_64/tests/stock.so build/boot-probe/x86_64/tests/missing.so
```

There are 14 bounded child-process cases: pointer/argument/callback forwarding,
diagnostic fields, missing/directory/FIFO/symlink markers, full and near-full
logs, writable or locked markers, preserved content, unavailable originals,
missing getters, self-reference, and concurrent forwarding with one record.
Each test child has a five-second alarm and a fresh temporary directory.

To run cross-built fixtures in an existing Linux VM image with musl:

```sh
python tools/boot_probe/run_vm_tests.py --arch aarch64 --qemu /path/to/qemu-system-aarch64 --kernel /path/to/kernel --initramfs /path/to/initramfs
```

The base initramfs must provide BusyBox and its architecture's musl loader.
The guest has no disks, network or USB passthrough. It mounts temporary
filesystems and powers down after the tests. Logs stay under the build directory.

An optional production-linkage test accepts a **privately supplied** Android
ARM64 sysroot containing `system/bin/linker64` and `system/lib64/libdl.so`:

```sh
python tools/boot_probe/run_android_vm_test.py --zig /path/to/zig --sysroot /private/android-sysroot --qemu /path/to/qemu-system-aarch64 --kernel /path/to/kernel --initramfs /path/to/initramfs
```

This runs the actual production proxy and marker paths, with the authored fake
stock getter under the stock path. Both libraries deliberately use the same
`gps.default.so` SONAME. The test checks Android symbol/dependency resolution,
unchanged interface/callbacks and marker creation by the test setup. It does
not run vendor GPS code or load a device SELinux policy. Supplied Android files
are copied only into an ignored local VM image; never commit or redistribute it.

## Before a separate hardware bench test

No install or reboot command is supplied. A reviewed bench plan needs to verify
the running build and consumer ABI, preserve the original library and its label,
hash-check the complete forwarding pair, and establish how the original can be
put back using access that actually works. A partition copy is not a tested
restore procedure. The marker should be prepared independently of activation.

Keep battery power through the immediate UI check and have the owner available
to confirm the normal display and controls. Do not depend on the owner knowing
a goggles-side GPS feature or interpreting an aircraft satellite indicator.

Compare the stock service's internal GNSS startup results before and after the
test, where the existing logs are accessible, and check that the service remains
running without repeated restarts. The inspected consumer logs the getter
interface's initialization and start return codes. Preserve those baseline
results rather than assuming a particular return code proves success. If the
logs cannot be read in the unchanged security state, record that verification
gap instead of treating responsive menus as evidence of GNSS health.

Successful initialization/start is distinct from receiving a satellite position
fix. The first marker test need not establish a fix, but must retain that limit
in its results. A fresh marker alone cannot validate GPS, display/input recovery
or flight operation. SELinux should retain its existing state throughout this
marker-only test.
