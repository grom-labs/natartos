# SD app runner

The standalone runner can discover directory packages, validate their
manifests and static executables, stage a verified internal copy, and run one
app at a time. Apps marked `"display": true` get the screen through the
Natartos overlay, the same way Doom and PC Stream do, and give it back when
they exit. The `mod_sd_apps.so` mod adds one More-menu entry per app on the
card. The Hello package draws a screen, reacts to the joystick, and records
each launch in its saved-data directory.

See the [launcher plan](SD_APP_LAUNCHER_PLAN.md) for scope. The
[initial bench](SD_APP_BENCH_2026-11-16.md) used an internal package. The
[2026-11-17 bench](SD_APP_BENCH_2026-11-17.md) installed the matched runner and
overlay, ran Hello from the SD card through More, and checked the missing-overlay
guard and cleanup after killing a runner. The menu launch returned through a
clean repaint; the deliberate runner-kill test used the existing UI-restart
fallback. More extensive menu/device validation remains separate.

This contribution is independent of optional boot activation: an existing
Natartos session is the runtime prerequisite. It ships development artifacts
and manual integration instructions; the default release installer does not
yet install the SD Apps mod and runner.

## Build

Upstream base: `82f1a7da1748f690eb1031bb391666ec09508278`.
The Windows build uses [Zig 0.15.2](https://ziglang.org/download/), with
`-target aarch64-linux-musl`, static linking, and no PIE. The Python helper
checks that Zig version and keeps its caches inside the ignored `build/` tree.
Pass the executable path from your extracted official Zig download:

```sh
python tools/sd_apps/build.py --zig /absolute/path/to/zig --framework
```

On Linux with `aarch64-linux-gnu-gcc` installed:

```sh
python3 tools/sd_apps/build.py --framework
```

Products are in `build/aarch64/`:

- `build-info.json`: compiler, upstream base, source hashes, and binary hashes
  for matching a tested build to the bench copy.
- `sd_app_runner`: the production runner, accepting static ARM64 ELF64
  `ET_EXEC` applications only.
- `apps/hello/`: the complete SD package, including `app.json`, `bin/hello`,
  and its asset.
- `mod_sd_apps.so`: the mod that registers one More-menu entry per SD app.
- `hook_menu.so`: the menu mod with the raised entry limit (32) that the
  per-app entries need; the released 0.1.0 menu allows 8.
- `hook_overlay.so`: the overlay with the SD display acknowledgement and
  ownership leases. Update this together with the runner for display apps;
  an older overlay cannot acknowledge them, so the runner refuses to launch.
- `natartos_loader.so` and `mod_template.so`: build-baseline checks when
  `--framework` is used; they are not needed for SD apps.

The runner and example use ordinary Linux syscalls through statically linked
libc, avoiding the goggles' Android linker/libc dependency. Apps that draw use
the header-only helpers in [`include/natartos_app.h`](../include/natartos_app.h).

The alternative Makefile is at `mods/sd_apps/Makefile`. CI builds a separate
`sd-apps-bench` artifact, and its release loop skips `sd_apps` (status
"in progress" in the README). Installing the runner under
`/vendor/natartos/apps/sd_apps/` needs installer manifest integration later;
Toolya's default upstream download will not supply it.

## Packages and commands

Place packages at `<external-card>/natartos/apps/<id>/`. The example's
[app.json](../examples/sd_apps/hello/app.json) defines the schema. Directory
name and manifest ID must match. IDs use lowercase ASCII letters, digits,
underscores and hyphens, starting with a letter or digit. Package paths use
ASCII letters, digits, dots, underscores and hyphens, separated by `/`.
Empty components, `.`/`..`, absolute paths, links, and special files are rejected.

The manifest supports `schema`, `id`, `name`, `version`, `entry`, and optional
`args` and `display` (`true`/`false`, default `false`; see
[Display apps](#display-apps)). Unknown or duplicate fields are errors. Names/arguments support valid
UTF-8 and JSON escapes; names cannot contain control characters and no string
can contain NUL. Maximums: 8 KiB manifest, 48-byte ID, 128-byte name,
64-byte version, 511-byte entry path, and 16 arguments of at most 255 bytes.
The parser rejects dynamic ELF interpreters/dependencies and incompatible
architecture before execution. Shared-library packaging is deferred.

Options precede the action. For example, on a bench with the observed SD path:

```sh
/data/local/tmp/sd_app_runner --apps /mnt/media_rw/sdcard0/natartos/apps list
/data/local/tmp/sd_app_runner --apps /mnt/media_rw/sdcard0/natartos/apps validate hello
/data/local/tmp/sd_app_runner --apps /mnt/media_rw/sdcard0/natartos/apps stage hello
/data/local/tmp/sd_app_runner --apps /mnt/media_rw/sdcard0/natartos/apps --timeout 10 run hello
/data/local/tmp/sd_app_runner clean hello
```

Without `--apps`, discovery requires exactly one FAT/exFAT mount under
`/mnt/media_rw/`, excluding `emulated`, and uses its `natartos/apps` directory.
It fails on missing or ambiguous mounts. Confirm the actual mount table before
using the example path. `list` emits one JSON object per valid package and
reports skipped invalid packages on stderr.

`menu-list` prints `id<TAB>version<TAB>name` lines sorted by name, for the
menu mod. It checks manifests and executables but skips the full-package hash
so it stays fast, reduces names to printable ASCII (the menu's string API is
ANSI), and stops itself after 5 seconds.

`validate` reads the SD package. `stage` prints the internal cache path after
verification. `run` stages and executes the opened internal executable with
the package as its working directory. It passes argv directly without a shell.
The child's environment contains only `PATH`, `LANG`, `NATARTOS_APP_ID`,
`NATARTOS_APP_PACKAGE` (the verified internal copy it runs from),
`NATARTOS_APP_SOURCE` (the package folder it was staged from, e.g. on the SD
card), and `NATARTOS_APP_DATA` (plus the display variables below); preload
hooks are not inherited. Standard input is `/dev/null`,
stdout/stderr go to the bounded log, and every other inherited descriptor is
closed, since the menu starts the runner from inside `dji_glasses`.

| Storage | Default |
| --- | --- |
| Verified packages | `/data/local/tmp/natartos-apps/packages/<id>/<sha256>/` |
| Saved data | `/data/natartos/appdata/<id>/` |
| Latest app output | `/data/local/tmp/natartos-apps/logs/last-run.log` |

`--cache` and `--data` accept separate absolute roots for testing. Source,
cache, and data roots must not overlap, and roots cannot contain symlinks.
The cache/data roots must belong to the current user and must not be writable
by other users. The device workflow expects the existing authorized root
session; the runner does not enable root, alter policy, or disable verity.

Packages are limited to 64 MiB and 512 files/directories, at most eight
directory levels below the package root. The package cache is capped at
256 MiB of file content, with a 2 MiB free-storage reserve before copying.
The single latest-run log is overwritten each run and capped at 1 MiB;
remaining output is drained. Saved application data has no imposed quota.

Copying uses a private partial directory, sorted SHA-256 tree fingerprints,
re-reading the staged copy, syncing files/directories, and an atomic rename.
File modes are normalized; only the entry executable receives execution
permission. Existing cache entries are re-verified before reuse. If one is
corrupt, `clean <id>` removes that app's cached versions before a retry.
It leaves saved data and unrelated directory names alone. Stale partial
directories for the selected app are removed before staging. Cache cleanup
does not require the SD card and is blocked while a runner is active.

The process lock permits one active runner. The app gets its own process
group; normal exit, crash, cancellation, and timeout trigger group cleanup.
SIGINT/SIGTERM/SIGHUP request termination, escalating to SIGKILL after two
seconds. Timeout is optional, defaults to none, and may be set up to one day.
The direct child is killed if its runner dies. Apps must remain in their
assigned group; daemonizing or detaching children is unsupported. These are
owner-selected privileged programs, with no app sandbox or trust implied by
hash verification.

Exit status is the app's status, `124` for timeout, `125` for runner errors,
or `128 + signal` for cancellation/crash. Inspect stderr and the latest log
to distinguish runner failures from an app returning the same code.

## Menu entries

`mod_sd_apps.so` is an ordinary Natartos mod. When the loader calls its
`natartos_mod_register` during `dji_glasses` start-up, it runs
`sd_app_runner menu-list` and registers one More-menu entry per app through
the existing `natartos_menu_register` API: the app's name, a subtitle with
its version, and the command

```sh
nohup <runner> run <id> >/dev/null 2>>/data/local/tmp/natartos.log &
```

Entry keys are `sd:<id>`, so a card app cannot replace another mod's entry.
The mod re-checks each id against the runner's id rules before placing it in
the command, and registers at most 24 apps. Runner errors (for example a busy
screen or SD permission denial) are appended to the shared Natartos log.

The runner is looked up at `/vendor/natartos/apps/sd_apps/sd_app_runner`
(future installer location), then `/data/natartos/bin/sd_app_runner` (bench).
The card is read once per `dji_glasses` start, so apps copied later appear
after the next `toolya natartos start`. Without a runner, an SD card, or with
an unreadable card, the mod registers nothing and the menu is unchanged.

## Display apps

With `"display": true` the runner uses the existing Doom/PC Stream frame format
with an additional SD session handshake:

1. Acquire the exclusive `fb_owner.lock` lease, shared across all runner cache
   paths. Refuse an active owner or a legacy app's `fb_override` with exit 125.
   A recognised SD frame whose owner lease is free can be reclaimed. Remove
   its flag and wait up to five seconds for the previous overlay's cleanup.
2. Create a fresh 16,588,864-byte `fb_shared` (mode 0644), write the SD protocol
   marker in its reserved header, then create `fb_override`. Wait up to five
   seconds for the overlay to acknowledge this exact frame inode. Its inject
   thread acknowledges only after finding a display buffer. Missing/old
   overlays and missing display buffers therefore fail with exit 125 **before
   the app starts or grabs any buttons**.
3. Stop the app (SIGTERM, SIGKILL after 2 s, exit 143) if `fb_stop` appears,
   `fb_override` disappears, or the overlay's lease is released by its death.
   Conversely, the overlay detects the runner's death through the owner lease
   and cleans up, even when SIGKILL prevented the runner's own cleanup.
4. After the app's process group is gone, remove `fb_override`. Keep the owner
   lease while the overlay repaints and holds `fb_overlay.lock`, so cleanup
   cannot remove a subsequent app's files. Wait at most five seconds; if the
   overlay is still busy, leave its files for cleanup and make the next launch
   wait for it. If both processes died, the next launch reclaims the stale SD
   files and requires a fresh acknowledgement.

The two lock files are persistent inodes and must **never be unlinked** during
cleanup or injection. The kernel releases their locks when a process dies;
their mere presence does not mean a session is active. Legacy Doom and PC
Stream frames have no SD marker and retain their existing protocol.

The files live in `/data/local/tmp` (`--display-dir` overrides it for tests).
Display apps also receive:

| Variable | Value |
| --- | --- |
| `NATARTOS_FB_PATH` | the `fb_shared` file to draw into |
| `NATARTOS_FB_WIDTH`, `NATARTOS_FB_HEIGHT` | `1920`, `1080` |
| `NATARTOS_INPUT_DEVICES` | colon-separated button devices (`--input`, default `/dev/input/event0:/dev/input/event3`) |

The app contract, implemented by
[`include/natartos_app.h`](../include/natartos_app.h):

- **Frames:** map the existing file; never resize, truncate or zero it. Draw
  BGRA pixels into the slot the overlay is not showing and then increment the
  frame counter (`nat_fb_publish`). Layout: 64-byte header with a `u32`
  counter at offset 0, then two 1920×1080×4 slots; the overlay shows slot
  `counter & 1`. Bytes 8–19 belong to the launcher/overlay handshake; apps
  must preserve the header apart from their frame counter.
- **Buttons:** open the listed devices and take an exclusive `EVIOCGRAB`, as
  Doom does, so the hidden DJI menu does not also react. `KEY_ENTER` is the
  5D press and `KEY_BACK` is BACK. The joystick reports its directions rotated
  180° (`nat_joystick` maps them to the wearer's view, matching the Doom port).
- **Exit:** return from `main`, ideally on BACK *release* so the release does
  not reach the DJI interface. Exit promptly on SIGTERM. Do not touch
  `fb_override` or `fb_stop`.

While an app holds the BACK button's device, the overlay's own hold-BACK
exit cannot see it (the same is true for Doom), so each app needs its own
exit. Toolya's stop (or `kill -USR1 $(pidof dji_glasses)`) still works.

## Tests

On Linux:

```sh
python3 tools/sd_apps/build.py --host-test --target x86_64-linux-musl
sh tests/sd_apps/test_runner.sh build/host
```

The SD-only suite has **75 integration checks**. On 2026-11-18 all 75 passed
in both x86-64 and ARM64 Linux QEMU guests with Zig 0.15.2/musl builds. The
production ARM64 runner, Hello, menu/overlay/SD mods and loader/template
baseline also compiled. No new hardware operations were part of this check.

The host-test build accepts its build architecture instead of always requiring
ARM64. Keep it out of device installation. Windows can cross-build it with
Zig by adding `--zig /path/to/zig`; run the resulting Linux executables in
Linux, not directly in Windows.

`tools/sd_apps/run_vm_tests.py` can bundle the same tests into an existing
Alpine Linux initramfs and run them in QEMU without an attached disk or network:

```sh
python tools/sd_apps/run_vm_tests.py --qemu /path/to/qemu-system-x86_64 \
  --kernel /path/to/vmlinuz-virt --initramfs /path/to/initramfs-virt
```

Use `--arch aarch64`, the matching QEMU/kernel/initramfs, and `--build` for an
ARM64 run. Test fixtures must also be built for that architecture. Locally,
the production ARM64 runner and Hello binary were substituted into the ARM64
test bundle. On 2026-11-16 the first 46 integration checks passed in both
x86-64 and ARM64 Linux guests. They cover malformed packages, path/link
rejection, ELF compatibility, cache reuse/corruption, storage budgets, saved
data, shell-literal argv, output limits, crashes, hangs, cancellation and
cleanup, plus a root path with a search-only ancestor.

The earlier 64-check suite covered the menu listing, the SD
Apps mod (`mod_check` compiles `mods/sd_apps/mod.c` for the host against
`tests/sd_apps/shim/natartos_common.h`, feeds it the runner's real listing and
crafted unsafe lines, and runs the command it registers), descriptor closing,
and the display hand-off driven through a FIFO standing in for the buttons:
BACK exit, a busy screen, both overlay stop paths, an app ignoring SIGTERM,
and a crash. All 64 passed on x86-64 Linux (WSL) with both the Zig/musl and
GCC 13/glibc builds. On 2026-11-17 the suite was extended to 76 checks, including
missing overlays/buffers, cancelled handshakes, stale acknowledgements,
legacy sessions, ownership across cache roots, killed runners, killed overlays,
and recovery after both die. The host overlay fixture uses the actual overlay
lease/acknowledgement helper with real file locks and a simulated repaint
delay. All 76 passed in both x86-64 and ARM64 Linux QEMU guests with Zig/musl
builds; the matched production ARM64 runner and overlay also compiled.
The SD-only contribution retains the same 75 SD integration checks and omits
the fork's unrelated mute check and mute artifacts. Its current validation is
reported with that count; the 76-check historical bench records remain intact.
The fixture does not emulate the DJI compositor. Host tests do not establish DJI
display, menu (Embedded Wizard) or SELinux behavior. Device evidence is limited
to the dated bench reports; other builds or units need their own checks.

## First device check

The [first console bench check](SD_APP_BENCH_2026-11-16.md) used an internal
source package. The [later bench](SD_APP_BENCH_2026-11-17.md) checked SD/menu
launching and display guards. The following procedure remains useful when
checking a different build or unit.
For subsequent checks, confirm the current firmware and mounts, and preserve
the matching baseline if the goggles have been updated.
Use a charged battery for the bench setup, outside flight operation. The
owner reported that PC USB power did not keep up with battery use during the
first session. Check the normal view/menu before the app run and immediately
afterward so a later power interruption does not obscure that observation.

1. Obtain the existing Toolya ADB session and release the browser's USB
   connection. Read the device identity, firmware, mounts, and execution context.
2. Transfer only the production runner to `/data/local/tmp/sd_app_runner`,
   set its mode to `0700`, and copy the complete Hello package to the confirmed
   SD app directory. Keep the SD executable's bytes unchanged; the runner
   will set permissions on the internal copy.
3. Run `list`, `validate hello`, and `stage hello`; capture their results
   before executing. Confirm that the returned path is under the intended
   `/data` cache.
4. Run Hello with a ten-second timeout. Inspect `last-run.log` and
   `/data/natartos/appdata/hello/launches.txt`, then repeat and confirm saved
   data is retained. A normal DJI display should remain responsive throughout
   this console-only check.
5. Capture any permission denial or crash before changing anything else.
   A SELinux failure is evidence to investigate, not a reason to toggle
   security automatically. Cleanup removes only the runner, its dedicated
   cache/data directories, and the Hello SD package if desired.

This console check did not exercise display ownership or the menu; see the
next section for those.

## SD access after disconnecting USB

For a bounded read-only check, `tools/sd_apps/sd_access_probe.sh` accepts
1–60 samples and checks SD access every two seconds. Start it with the existing
`setsid` tool, redirect stdout/stderr to a dedicated internal log, and verify
its `PROBE_START` marker before unplugging. Reconnect afterward to retrieve
the log and check `PROBE_FINISHED`. The USB configuration property alone is
not a physical connection indicator; correlate samples with host connection
events and the owner's observation.

The first such test completed all 60 samples and still received permission
denials during unplugged operation. That session had no Natartos session,
so SELinux was presumably enforcing. With Natartos started (Toolya sets
SELinux permissive for the session; `/sys/fs/selinux/enforce` reads `0`), the
card root at `/mnt/media_rw/sdcard0` is readable from the same `su` context
that Natartos runs `dji_glasses` in. SD launching therefore depends on an
active Natartos session. This probe does not change security settings. See
the [bench results](SD_APP_BENCH_2026-11-16.md).

## Next device check: menu entries and display

Initial Stage A was performed on 2026-11-16: Hello was shown, its joystick
mapping matched DJI's menu, and BACK exited, but repaint used the UI-restart
fallback. The 2026-11-17 bench subsequently checked menu launches and display
guards. The checklist below is for repeating those checks on another build or
unit. Use a charged battery outside flight operation, with firmware and mounts
re-confirmed. Nothing here writes to `/system`.

**Stage A: display hand-off from ADB.** Requires an active Natartos session
with the updated overlay mod (`toolya natartos start`). Installing that overlay
is a separate `/vendor` change; agree the device steps before doing it. The
log `/data/local/tmp/natartos.log` survives a `dji_glasses` restart, so
confirm its latest loader start (`hook_overlay loaded`, `HOOK ALIVE`) is newer
than the current `dji_glasses` (modification time of `/dev/input/event3`).
The updated runner refuses a display app if the overlay does not acknowledge
within five seconds, before any button grab. The old log-age check remains
useful context but is no longer the launch precondition.

1. Replace `/data/local/tmp/sd_app_runner` with the new build (check its
   SHA-256 against `build-info.json`) and copy the new Hello package, whose
   manifest has `"display": true`, to `/data/local/tmp/natartos-bench/apps/hello`.
2. Confirm `/data/local/tmp/fb_override` does not exist, then run
   `sd_app_runner --apps /data/local/tmp/natartos-bench/apps --timeout 60 run hello`.
   The timeout is a safety net: it stops Hello and returns the screen even if
   no button works.
3. Check: Hello appears; up/down/left/right move the highlight the way the
   joystick is pushed (if inverted, fix `nat_joystick` only); the 5D press
   changes colour; BACK returns a responsive DJI interface. Repeat three times.
4. If the screen stays on Hello: `touch /data/local/tmp/fb_stop` from ADB, or
   wait for the timeout. The overlay restarts `dji_glasses` if its repaint
   fails.

**Stage B: menu entries.** Needs the new `hook_menu.so` (32 entries) and
`mod_sd_apps.so` in `/vendor/natartos/mods/` with
`/vendor/natartos/enabled/mod_sd_apps.so`, and the runner at
`/data/natartos/bin/sd_app_runner` (directory owned by root, mode 0700).
Install them with Natartos's own mechanism (Toolya local manifest) and keep a
copy of the released `hook_menu.so` for rollback; agree the exact steps
before writing under `/vendor`.

1. After `toolya natartos start`, the log should show `[sd_apps] SD app
   entries: N` and `[menu] register: added` for each `sd:` entry.
2. Open More: each app appears after Doom and PC Stream. With more than
   eight entries, confirm the list scrolls and every entry launches.
3. SD access: the earlier root session was denied (see above). If the
   listing reports a permission error under the Natartos session, stop and
   investigate the session's permission setup rather than changing policy.
4. Rollback: restore the released `hook_menu.so`, remove `mod_sd_apps.so`
   and its `enabled` marker, and start Natartos again.
