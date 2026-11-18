# Boot activation development bench â€” 2026-11-18

Status: isolated tests, enforcing qualification, real warm fallback, and the
guarded enforcing cold permission transition with GUI health acknowledgement
passed. The owner confirmed visible Natartos menus after a separate unplugged
boot without Toolya, followed by a successful Hello launch reporting SD-card
execution. The initial USB OTG banner cleared at the GUI restart; its cause and
the earlier boot's empty SD scan remain unexplained. This follows
the [cold marker bench](BOOT_MARKER_BENCH_2026-11-17.md).

## Read-only live preflight

The owner kept the retail build 35808 goggles connected in the existing Natartos
session. Host ADB could see them when sandboxed ADB could not. Targeted metadata
confirmed the expected kernel, slot and boot-image digest. `/proc/kallsyms` was
empty despite `kptr_restrict=0`; no runtime symbol anchor was available.

A write-disabled static ARM64 preflight helper was staged in a private temporary
directory. It checked the full boot-image hash and read the two candidate RAM
words documented by the inspected Toolya source. One candidate matched the
permissive signature and direct enforcement value; the other was rejected.
The service retained its PID and enforcement remained zero. This is a permissive
session observation, not yet enforcing-boot qualification of the new helper.

Live device-node labels were `device` for `/dev/mem` and input event0, and
`block_device` for the boot partition. The static CIL allow model supplies the
needed read/ioctl permissions for those labels in `dji_services`; it contains
no permissive domain declarations. This is distinct from a loaded-policy access
test. Exact profiles, addresses, raw logs and unit identifiers remain private.

## Offline validation

The source includes separate read-only qualification and activation-capable
artifacts. The qualification build excludes the permission-write function and
GUI-control paths. Linux fixtures exercise profile integrity, state handling,
bypass, supervision, health and process handoff. Optional Android-linker checks
exercise the production proxy, detached handoff and production loader's health
message using authored stand-ins. See `build/boot-activate` logs and build-info
files for local hashes and exact execution results. Both ARM64 and x86-64 passed
24 supervision/qualification scenarios, seven raw-handoff scenarios and the
profile/hash/byte-boundary matrix. The 14 existing marker cases also passed on
each architecture. Android production-proxy, handoff and loader-message checks
passed using the privately supplied linker and authored fixtures.

## Write-disabled warm qualification

Only the forwarding proxy, read-only qualifier and private profile were deployed.
The new activation supervisor and loader were not installed. The qualifier was
placed at the helper's fixed production name in a new protected vendor directory;
its mode and `vendor_file` label were verified. Its state/log directory is root
mode `0700`, with protected `system_data_file` files.

| Warm bench artifact | SHA-256 |
| --- | --- |
| Optional forwarding proxy | `2c863c27c5d88ea98be0fe6250f4e0d76cd52d2f1aad2e1bcc0b4a480a40399f` |
| Write-disabled qualifier | `8bc3213c797b075bf9c266d0a088680f320b3bacd96f9521ea66565658c0f554` |

These warm artifacts correspond to source checkpoint `80993d9`; later offline
changes below were not deployed in this test.

The qualifier passed first in a root-shell session. A temporary software disabled
flag then caused a successful bypass. After the controlled GPS-library swap and
`dji_sys` restart, the proxy forwarded the stock getter and handed off to the
qualifier. A fresh `mode=boot` record from the current boot passed all gates,
including the exact `dji_services` context, input-state query, full boot-image
digest and matching enforcement/RAM signature. It reported `enforcing=0`,
`memory_writes=0` and `ui_changes=0`. `/system` and `/vendor` were read-only.

The service restart required refreshing the existing installed Natartos injector
for a clean display, as in the marker bench. Captures then confirmed menu open,
navigation and BACK return to the normal HUD, with battery at 92%.

The original GPS library was restored afterward and its hash matched the saved
stock pair. The qualification enabled flag was removed. The existing installed
Natartos loader remained unchanged; the new vendor helper/profile are dormant.
Read-only mounts were checked again. After refreshing the existing injector,
menu open/navigation/BACK and the normal HUD were verified again. No active boot
hook was left by this warm test. A detached current-boot restoration timer had
also been armed; it is not protection across a power cycle.

## Subsequent offline review

Command-output limits and child/watcher waits were tightened, and GUI process
groups are now signalled while their leader's PID remains reserved with
`WNOWAIT`. The watcher also retries a briefly held lease after pipe EOF: ARM64
emulation exposed a race where stock restoration succeeded but the failed-attempt
record was missed during parent cleanup. The helper now allows a bounded lease
release interval before recording failure. Profiles are read through EOF, and
the raw handoff preserves redirected standard descriptors across exec.

Both architectures passed the updated 25 supervision/qualification scenarios
and seven handoff scenarios, plus the profile matrix. This includes excessive
command output, a GUI ignoring TERM, and five successive supervisor-kill/recovery
cycles. Android proxy/handoff/loader-message checks passed after the handoff
change. The hardware remains on the restored original GPS library.

## First enforcing cold qualification and correction

After the owner power-cycled and used Toolya Enable ADB then Root without
starting Natartos, the GPS proxy produced a fresh `dji_services` marker with
`enforcing=1`. The owner reported a normal display. The helper logged
`startup bypass` before reaching its permission probe, so this did **not**
qualify the memory-read path. The existing log combined input-access failures
and BACK state; it did not prove that the owner held a button.

The mandatory input gate was removed from qualification and activation.
Boot decisions now use persistent configuration and automatic checks. Profile
errors are also preserved in the bounded boot log. Updated tests passed on both
architectures, and both boot executables are audited for absence of input-device
paths. See [the debug entry](../DEBUG_LOG.md) and the later decision entry.

The owner started Natartos normally for cleanup. The original GPS pair was
restored and verified before the corrected read-only helper was installed.
Its SHA-256 is `2a3ba6283bc7094424426ec4643665462b0367a9593e528cb3aeee56c1247396`.
It passed a live warm qualification without input-device access. The same
previously validated forwarding proxy is being used for the repeated cold test;
the activation-capable helper and new loader are still not installed.

## Corrected enforcing cold qualification

The owner power-cycled again, reported a normal display, and completed Toolya
Enable ADB then Root without starting Natartos. A new live boot ID matched both
the GPS proxy marker and one fresh qualifier record:

```text
BOOT_QUALIFY_OK mode=boot ... enforcing=1 word=0x01000100 memory_writes=0 ui_changes=0
```

This confirms that the corrected static helper executed in the required root
`dji_services` context and passed the exact kernel/slot/full-image checks,
boot-device size check, read-only physical-memory access, state signature and
direct enforcement agreement under enforcing policy. No input-device check
participated. Current `/system` and `/vendor` mounts were read-only. The root
`su` shell could not discover the service PID with `pidof`; the fresh native
records and init service property supplied the relevant evidence instead.

The activation-capable helper was not deployed. The permission-byte write,
automatic modified-GUI startup and real-device failure fallback remain untested.
This qualification also does not establish GPS position fixes or flight operation.

After evidence collection, the owner started Natartos normally for cleanup.
The original GPS pair was restored and hash-verified, the qualification enable
flag removed, the existing installed loader verified unchanged and read-only
mounts checked again. The new helper/profile remain dormant without an active
boot hook. Menu open, navigation and BACK response were observed. Capture selects
the first nonblank buffer and retained a fading-menu frame, so it does not alone
confirm the current optical HUD. The owner had confirmed normal cold display;
the final control captures are recorded with the private bench logs.

The fixtures do not load a device SELinux policy, execute real GPS callbacks,
run the DJI GUI or establish a usable device recovery procedure. Full activation,
its real-device fallback, visible display/controls and flight operation still
need separate bench evidence. The next hardware checkpoint is controlled
activation using the qualified profile, including an actual overlay health
acknowledgement and stock fallback, before an unattended activation cold boot.

## Full warm activation and real stock fallback

A controlled `--session` test installed the full supervisor, health-reporting
loader and a manifest for the existing seven enabled mods. It required the
already-permissive state, so no permission-memory write occurred. The original
GPS library stayed in place throughout warm testing. Installation directories
and enabled markers were protected against group/world writes; original content
and metadata were backed up. Current-boot emergency timers restored the stock
loader and original metadata between the first two failed attempts.

The first warm attempt loaded the overlay and reached its frame hook, but the
supervisor timed out without its health packet and restored the stock init GUI.
Matching the stock GUI's fixed Android environment did not fix that failure.
Added diagnostics showed `boot health: mode=0`: libc environment lookup did not
expose the health settings, and the loader had selected its manual health path.
The exact reason for unavailable libc lookup was not established.

The corrected loader falls back to a bounded read of the initial environment
from `/proc/self/environ`. With the same acknowledgement requirement it logged
`boot health: mode=1` and `boot health: report bytes=24`; the supervisor recorded
`healthy` with zero failed boots. The GUI mapped the intended loader and overlay.
Menu open, navigation and BACK-to-HUD responses were captured at 92% battery.
Deliberately killing the supervisor then caused the independent watcher to
restore the stock init GUI; its maps contained neither loader nor mod preloads.
Both system and vendor mounts remained read-only. The warm timeout was cancelled.

| Successful warm artifact | SHA-256 |
| --- | --- |
| Full supervisor | `7fdc5fdf3e4b9cd98e49497d7f0377b3680218d3cdd8102c1885162e2234b7b6` |
| Health-reporting loader | `b01b05253d167395f0571660b22e88392a65552c8e8d15b322956839926c573f` |
| Matching activation proxy, staged for cold test | `64766260b132c66bfabacede76e7829a74d2c2b4ccafac9484ecf68ad96ef490` |

Both Linux architectures passed the profile matrix, 25 supervision/qualification
and seven handoff scenarios after the fix. The fake GUI now exercises unavailable
getenv, and the Android-linker fixture supplies real initial environment fields
while its getenv stand-in returns null. The Android production-loader pipe check
passed. These fixtures still do not establish actual Bionic thread behavior.

The next checkpoint is the actual guarded enforcing-to-permissive transition
and automatic modified-GUI startup after a fresh power cycle. The warm result
alone does not validate that transition or GPS operation.

## First full cold-test handoff: installation timer expired

The next reported power cycle did not exercise activation. Its fresh boot ID
was accompanied by stock GPS hashes, an absent enabled flag, the previous boot's
unchanged state, no fresh proxy marker and no activation attempt. The saved
current-boot installation timer recorded `RESTORE_DONE`: it restored stock GPS
and removed the opt-in before that power cycle. This is a bench handoff error,
not evidence for or against enforcing activation. The owner's display was normal.

For the repeated handoff, the installation timer is explicitly cancellable and
its cancellation must be confirmed before USB is released. No deadline is placed
on the owner's reboot step. The artifact set remains the successful warm set.

The repeated re-arm reached healthy state through the real proxy handoff in the
already-permissive session. A host-script GUI-shutdown race was corrected with
a bounded exit wait and unconditional stock-GUI restart on its error path. Both
installation timers recorded `CANCELLED` before the repeated owner handoff.
The actual enforcing-to-permissive cold transition remains the next checkpoint.

## Repeated enforcing cold boot: permission transition and GUI health

After the owner power-cycled and used only Toolya Enable ADB and Root for
inspection, a new boot ID matched a fresh enforcing proxy marker, the full
supervisor's preflight and its healthy state. The helper recorded an enforcing
signature before applying the guarded permission transition and confirmed the
permissive signature afterward. Direct enforcement readback was zero. The
state had zero failed boots and phase `healthy`. The modified GUI's parent was
the fixed helper with `--boot` in the expected service context; its maps included
the pinned loader and overlay. All installed candidate and original-backup
hashes matched. Current system/vendor mounts were read-only, the enabled flag
remained present and no manual injector PID file was present.

The loader reported boot health mode one, seven loaded mods and a complete
24-byte acknowledgement. Menu opening, navigation and BACK-to-HUD were captured
in this session at 92% battery. The owner reported a normal display. The
permission transition and frame-hook acknowledgement are established for this
exact build; they do not establish GPS fixes or flight behavior.

The owner then clarified that More had shown no Natartos entries before using
Toolya, and that an unplugged boot had displayed USB OTG mode. A timing check
using the kernel's `AT_CLKTCK=100` placed the supervisor at 14.02 seconds, the
modified GUI at 32.07 seconds and adbd at 75.86 seconds after boot. This proves
that this modified GUI process began before ADB, while leaving the timing and
visibility of the owner's earlier menu check unresolved. A repeated unplugged
boot with a menu check after 60 seconds is the next direct observation.

The SD module also registered zero app entries at cold initialization: the
runner reported that it could not find exactly one external FAT/exFAT mount.
The card was mounted in both the root shell and GUI mount tables by the later
inspection. This does not by itself establish the cause or prove discovery
recovered. The same automatic session remains enabled; no runtime or persistent
USB settings were changed during this investigation. ADB was stopped for the
owner's unplugged check.

## Owner-confirmed unplugged startup

The owner repeated the power cycle with USB unplugged and checked More before
using Toolya. They reported an initial USB OTG indication, then a restart of the
glasses application roughly ten seconds after the initial display. After that
restart the banner was gone and Natartos menu entries were present. This is an
owner observation, complementing the preceding instrumented boot's permission,
process and health evidence. It confirms usable automatic menu startup without
Toolya or a USB connection. The visible application restart matches the
supervisor's replacement of the stock GUI; it is not evidence of a second
whole-device reboot.

The reason for the initial OTG indication is not established. It did not persist
into the owner-confirmed Natartos session. SD application discovery and GPS fix
behavior remain outside this confirmation. The existing automatic bootstrap is
left enabled, and host ADB remains stopped. No additional device changes or
reboots were requested for this confirmation.

## Owner-confirmed Hello launch from SD after automatic startup

Following the unplugged startup confirmation, the owner reported successfully
launching Hello; the application reported that it was running from the SD card.
This extends the owner-observed result from visible Natartos menus to a working
SD application launch after automatic startup. No further code or device changes
were made between these confirmations.

The earlier instrumented boot's zero-entry scan is retained as a separate
observation. This successful launch establishes usable SD discovery and execution
in the owner's reported session; it does not explain the earlier empty scan or
establish identical discovery timing on every boot. The remaining open evidence
gaps are that scan's cause, the transient initial OTG indication and GPS fixes.

## Upstream PR preparation

The optional loader health path is now gated by `NATARTOS_BOOT_ACTIVATION`.
The experimental builder opts in; the default release build omits it. A
preprocessor comparison matched the opt-in loader to the bench-tested source
and the default loader to upstream `master`. All 18 allocated ELF sections
(excluding note metadata) matched a rebuilt bench-source reference. The full
shared-file hash changed, so the artifact table above continues to identify
the actual installed bench binaries, not a new hardware deployment. The
supervisor and activation-proxy hashes stayed unchanged. No device writes were
made during PR preparation.

Both architectures passed fresh runs of the 25 supervision/qualification,
seven handoff and 14 marker scenarios, plus the profile matrix. The optional
Android proxy/handoff/loader checks passed. A separate workflow now cross-builds
and audits activation artifacts on both architectures with checksum-pinned Zig;
its YAML was checked locally. A hosted workflow run still requires publishing
the branch. Public CI does not require or redistribute private Android inputs,
production profiles or device captures.
