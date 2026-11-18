# Experimental boot activation

Work on `codex/boot-activation` builds on the separately validated marker branch.
The default marker build still has no activation behavior. These optional
artifacts are outside release packaging and installation flows.

The implementation has passed authored Linux fixtures and Android-linker
checks, a real warm launch with killed-supervisor fallback, and an enforcing
cold permission transition with GUI health acknowledgement. The owner also
confirmed Natartos menu entries after an unplugged boot without using Toolya.
The owner subsequently launched Hello, which reported running from the SD card.
An initial USB OTG banner disappeared at the glasses-application restart; its
cause and the earlier boot's empty SD scan remain unexplained. See the
[dated bench record](BOOT_ACTIVATION_BENCH_2026-11-18.md) for the exact stage.

## Components

| Artifact | Behavior |
| --- | --- |
| `gps.default.so` | Forwards the stock getter and appends the existing marker. With explicit opt-in, hands off to a fixed static helper in `dji_sys` only. |
| `boot-preflight` | Reads a supplied profile, full boot-image digest, enforcement state and one RAM word. Its memory-write function is compiled out. |
| `boot-qualify` | Runs the real boot gates and read-only permission check. Memory-write and GUI-control paths are compiled out. |
| `boot-supervisor` | Checks the profile, optionally enters runtime permissive mode, then supervises the modified GUI and stock fallback. |
| `natartos_loader.so` | Adds an optional inherited-pipe acknowledgement after the existing overlay frame-hook signal becomes positive. |

`boot-qualify` is installed at the helper's fixed name during qualification;
the activation-capable supervisor remains a separate artifact. Link-only
`libdl.so` inputs and all test executables must never be installed.

The GPS ABI, original-library requirement and invalid-installation failure
modes remain those of the [marker experiment](BOOT_MARKER_PROBE.md). Neither
the constructor nor the helper changes GPS callbacks. A working constructor
does not establish GPS operation or a satellite position fix.

## Handoff and bypass

The optional proxy requires root, `/system/bin/dji_sys` as argv[0], a protected
helper directory/file, the exact enabled-file content below and no global
disabled flag. Two raw forks detach the helper; the constructor reaps only the
short handoff child. Fork children make raw syscalls until `execve`, avoiding
inherited libc/linker locks. The helper receives a fresh minimal environment
and closes inherited descriptors. Scheduling and filesystem access do not have
a hard wall-clock deadline.

Production paths:

```text
/vendor/lib64/natartos_boot/boot-supervisor
/vendor/lib64/natartos_boot/profile
/vendor/lib64/natartos_boot/manifest
/data/dji/natartos-boot/enabled
/data/dji/natartos-boot/disabled
/data/dji/natartos-boot/boot.log
/data/dji/natartos-boot/state
```

The state directory must be root-owned and protected, normally mode `0700`.
Prepare `boot.log` as root mode `0600`; logs stop appending at 128 KiB. The
enabled file must be root-owned, without group/world writes, and contain exactly
`natartos-boot-v1` followed by LF. Profiles, manifests and code must also be
root-owned, without group/world writes or final symlinks. Their ancestor
directories must be trusted. No production profile is provided.

Either `/data/.natartos_disabled` or the new `disabled` flag skips startup.
A ten-second delay lets stock startup settle; the helper continues checking
those flags throughout it and again before a permission write. Boot decisions
use persistent configuration and automatic guards. Neither qualification nor
activation reads input-device state or requires a button action. During an
active session, either software flag requests a return to stock.

The activation helper also refuses an active/advisory manual injector PID and
the legacy loader's safe-mode counter. Do not start Toolya's Natartos injector
concurrently with an automatic session.

## Exact-build permission profile

The profile format is deliberately strict: every key occurs exactly once,
unknown keys are rejected, and the file ends with LF.

```text
schema=1
release=EXACT_KERNEL_RELEASE
version=EXACT_KERNEL_VERSION
slot=1
image_sha256=FULL_64_LOWERCASE_HEX_DIGEST
state_phys=0x0
```

This example is intentionally invalid. There is no address search, default
kernel address or fallback profile. A separate read-only calibration must
establish a real profile for the exact running build. Profiles and device
evidence stay in the owner's private research workspace.

The helper verifies `uname`, one unambiguous `androidboot.slot_suffix` token,
the active 32 MiB boot partition's full SHA-256, a normal-RAM aligned address,
the `/dev/mem` character-device identity, the expected state word and agreement
with `/sys/fs/selinux/enforce`. Only words `0x01000100` (enforcing) and
`0x01000000` (permissive) are accepted. These signatures and byte offset follow
the inspected Toolya contract; this is a device-specific mechanism.

The initial check opens physical memory read-only. Immediately before an
enforcing-to-permissive transition, identity and state are checked again, as
are the bypasses. Only then may a writable descriptor be opened. A final state
read precedes a one-byte zero write at the profiled address plus one. Both the
word and enforcement file must then report permissive. A failed check never
tries another address. An already-permissive, matching profile requires no write.

This does not change the on-disk policy or boot image. It cannot attest to all
runtime kernel modifications. Once entered, permissive mode lasts until reboot,
including after a failed mod startup or a return to stock. The helper does not
try to restore enforcing mode around an already-modified running process.

## Session and recovery behavior

An exclusive lease prevents duplicate supervisors. A durable atomic state record
is saved before activation. Each boot is attempted at most once; three boots
without a successful frame-hook acknowledgement block further attempts until
the owner reviews/resets state. Missing initial state is allowed; malformed,
untrusted or unwritable state blocks activation.

A separate manifest pins the loader followed by the enabled mods, sorted by
filename, with one `SHA256 absolute-path` line each. The helper requires an
enabled overlay, exact agreement with enabled markers, protected installation
directories and matching file hashes. It validates again before each spawn.
This is a separate manifest from Toolya's existing release metadata.

The helper stops the init-managed GUI using the observed service property, then
executes a new GUI with the loader and complete enabled preload list. Startup
requires a 24-byte message containing a per-launch nonce and the child PID.
The loader sends it only after the existing overlay frame-hook signal becomes
positive. Boot mode never resets health solely because a process remains alive.
The GUI receives the fixed standard Android environment observed on the stock
service. If libc environment lookup is unavailable, the loader reads a bounded
initial environment from `/proc/self/environ`, rejecting malformed or duplicate
health fields. It records health only after a complete pipe write. The legacy
manual-injection health path remains unchanged.

The overlay signal proves its frame-hook code ran; it does **not** prove a
visible, correct image, functioning controls or continued rendering. Human/UI
bench checks remain necessary. The one-time acknowledgement does not detect a
later frozen display.

Missing/invalid health within 30 seconds returns to stock. Crashes and requested
restarts both consume a maximum of three launches per 120-second window.
The GUI has its own process group and a parent-death kill signal. A separate
pipe watcher restores the init service on normal exit or supervisor death.
Failures after a prior acknowledgement are recorded as failed when possible.
Commands, service-state waits and child termination have bounded waits.

The watcher is a best-effort process fallback, not an independent recovery
system. Killing all participants, power loss, denied property access or a
broken stock service can defeat it. The software disabled flags skip activation;
they do not bypass an invalid GPS forwarding pair. No tested firmware restore
or flight-safety claim is made by these fixtures.

## Build and tests

```sh
python tools/boot_activate/build.py --zig /path/to/zig-0.15.2 --arch aarch64
python tools/boot_activate/build.py --zig /path/to/zig-0.15.2 --arch x86_64
```

Outputs and source/artifact hashes are under `build/boot-activate/<arch>`.
Native helpers must have no dynamic imports, dependencies or ELF interpreter.
The optional ARM64 shared artifacts receive symbol/dependency and stack audits.
No vendor code is required to build them. The builder explicitly defines
`NATARTOS_BOOT_ACTIVATION` for the experimental loader. The default `src/Makefile`
build excludes boot health entirely; `manual/natartos_loader.so` is also built
and audited to check that separation. Do not substitute that default loader in
the supervisor's manifest: it does not send a boot health acknowledgement.

The separate `boot-activation.yml` workflow cross-builds and audits both native
architectures using checksum-pinned Zig 0.15.2. It requires no firmware, USB or
production profile. Execution of the VM suites below is separate from those
CI build checks; optional Android checks require the private inputs described
below.

```sh
python tools/boot_activate/run_vm_tests.py --arch aarch64 --qemu /path/to/qemu-system-aarch64 --kernel /path/to/kernel --initramfs /path/to/initramfs
python tools/boot_activate/run_android_vm_test.py --zig /path/to/zig --sysroot /private/android-sysroot --qemu /path/to/qemu-system-aarch64 --kernel /path/to/kernel --initramfs /path/to/initramfs
```

The Linux guests have no disks, network or USB. Fixtures cover SHA-256 vectors,
profile and state rejection, exact byte-write boundaries, bypasses, health
messages, retries, duplicate instances, supervisor death and raw handoff.
The optional Android test uses privately supplied linker64/libdl with authored
GPS/overlay fixtures. Its synchronous pthread stand-in and deliberately unavailable getenv exercise
the production loader's linking, initial-environment fallback and health protocol,
not actual Bionic thread behavior. No
captured firmware is redistributed.

Hardware installation is a separate reviewed bench operation. First qualify
the helper without memory writes or GUI control, preserve and hash-check the
stock GPS pair, verify the restoration path and check display/controls after
service changes. A timer in the current boot is not recovery after power loss.
