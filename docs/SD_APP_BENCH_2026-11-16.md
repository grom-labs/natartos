# First console bench result — 2026-11-16

The production ARM64 runner executed the console Hello application twice on
the retail goggles, with both runs exiting successfully. Saved data survived
the second launch and subsequent cache cleanup. This test used an **internal
source package**, not the SD card.

## Observed configuration

- Firmware/kernel build: 35808, matching the previously collected firmware
  baseline. Root shell context: `u:r:su:s0`.
- `/system`: read-only through `dm-0`; `/vendor`: read-only direct mount.
- `/data`: writable ext4; execution of the static runner and Hello succeeded
  with the existing session permissions.
- External SD: mounted exFAT with `noexec`; direct access from this root
  session was denied. USB ownership was raised by the owner and remains to
  be separated from the access-policy question. Reported USB configuration
  was `acm,adb`; no USB configuration changes were made.

## Commands and results

The runner was transferred to `/data/local/tmp/sd_app_runner` and its SHA-256
verified before execution. The Hello package was transferred to
`/data/local/tmp/natartos-bench/apps/hello`; its three files were also checked
against the local build. Source files were readable but not executable.

Using `--apps /data/local/tmp/natartos-bench/apps`:

| Check | Result |
| --- | --- |
| `--help` | Runner starts on the goggles. |
| `list` | Hello discovered. |
| `validate hello` | Static ARM64 ELF and package accepted; 37,392 package bytes. |
| `stage hello` | Verified internal copy published successfully. |
| `--timeout 10 run hello`, twice | Both returned `exit=0`, with 231 log bytes each. |
| Saved data | Two success records in `/data/natartos/appdata/hello/launches.txt`. |
| `clean hello` | Cached package removed; saved-data SHA-256 unchanged. |
| Stage after cleanup | Same package fingerprint published again. |

Package fingerprint:
`de25bccae421f3c68e8f341ccef3915c5424de1cd4e3e6484cce36b061bd23ed`.
Tested fixed runner SHA-256:
`76e286632c220bbd2806c5c4d2d36b8b00afd266ad3505daaed20a27b1234dd9`.

The first attempt found an unnecessary directory-read requirement in our
runner. See [the debug entry](SD_APP_DEBUG.md). The failure was reproduced
in Linux, fixed, and the regression suite passed for both x86-64 and the
production ARM64 runner before the corrected executable was uploaded.

The Hello log's bundled message refers to an SD package; it does **not** prove
that this test read the card. The package was supplied from internal storage
to avoid changing SD contents while ownership/access was unresolved.

## Owner-reported reboot afterward

The owner reported that the goggles rebooted after the battery drained faster
than the PC could charge it, and that the subsequent boot appeared normal.
The cause was not independently measured or confirmed from shutdown logs.
The owner had not been able to check the menus immediately after Hello.
This is encouraging evidence about boot behavior after the test, but it is
not a controlled reboot/recovery test or confirmation of menu responsiveness.

The owner also tried ejecting the SD card from the PC and reported no known
goggles UI control to disconnect USB storage. A subsequent local ADB check
found no device, so SD access, the new boot's security context, and persistence
of the test files could not be rechecked. Further device testing needs a
charged battery and a restored ADB connection; no additional app run was made.

## Reconnection and SD check without USB

The owner clarified that the ADB absence followed intentional unplugging,
and that the goggles display storage details when USB is disconnected.
After another rooted connection on build 35808, the runner and Hello binary
still matched their recorded SHA-256 values, and both saved success records
were present. This confirms that those test files survived the reboot; it
does not establish an automatic startup mechanism.

A one-shot [read-only shell probe](../tools/sd_apps/sd_access_probe.sh) was
copied into the dedicated internal bench folder. Its syntax, argument bounds,
and one-sample behavior were checked before it ran in a detached session.
It attempted to enter and list the SD root every two seconds, discarding
filenames and saving only access status/errors to an internal log. It neither
wrote the SD card nor executed code from it.

The owner unplugged USB, waited, checked the storage view/menus, and
reconnected. The probe completed all 60 samples; every attempt returned
`Permission denied`. Twelve samples had recorded UTC times inside the
host-observed ADB-disconnected interval. The probe's root context was still
`u:r:su:s0`. Therefore USB sharing alone does not explain this session's SD
access denial. SELinux policy is a likely contributor; no live AVC trace was
obtained (`logcat` is absent and `dmesg` access was denied).

The checked-out Toolya CLI explicitly calls `ensure_permissive` when starting
Natartos (`cli/src/natartos.rs`, start path), using its session kernel-memory
helper if needed (`cli/src/selinux.rs`). That code has only been inspected;
no permission toggle or Natartos start was performed during these tests.
The current standalone root test must not be presented as verification of
the framework's runtime permission setup.

The owner confirmed normal menus after the unplug/reconnect check. A third
console Hello run after the reboot then returned `exit=0` and appended a
third saved record. The owner immediately confirmed that the normal menus
and controls were still responsive after that run.
The one-shot SD probe has finished and has no reboot/startup hook.

## Changes and remaining checks

Device writes were limited to the runner, the internal source fixture,
one-shot probe script/log, cache/log folders, and Hello saved data under
`/data`. No Natartos mod was
installed or injected. No services were restarted, firmware partitions
written, filesystems remounted, security settings changed, or SD files written.
The protected mounts still reported read-only after execution.

The small test installation remains available for inspection and the next
bench check. It has no automatic startup trigger. Local evidence includes
command outputs, transfer hashes, the pre-fix binary and build record, the
failing regression result, and the fixed build record; device-specific
captures stay outside the public fork.

The console execution and immediate menu check succeeded. Direct SD
discovery and loading, the graphical picker, display/input ownership, phone
activation, and automatic launcher startup after reboot have not yet been
validated by this test. The owner's successful-boot report above does not
establish those launcher behaviors.

## Display hand-off check (Stage A) — afternoon

The owner installed Natartos with Toolya at 14:42 (six default mods under
`/vendor/natartos`: loader, background, menu, overlay, popen, Doom, PC
Stream). The install ran `inject.sh` once; `dji_glasses` later restarted
without Natartos at 14:47:49 (cause not recorded). The owner had not started
Natartos for this test. Nothing in these steps wrote to `/system`.

The new runner (`ad4179f8…`) and the display-mode Hello package (manifest
`"display": true`, `bin/hello` `a6a6e52a…`, package fingerprint
`b546a31d…`) were transferred under temporary names, checked against
`build-info.json`, then swapped into `/data/local/tmp/sd_app_runner` and the
**internal** bench folder `/data/local/tmp/natartos-bench/apps`. `menu-list`,
`validate` and `stage` succeeded there. Both button devices (`gpio_keys` on
event0, `omc_keys` on event3) opened from the ADB root session.

**SD access:** at about 14:50, between the stock restart and the owner's
Natartos start, `sd_app_runner menu-list` with automatic card discovery
failed with `open directory: Permission denied`. Neither run below read the
SD card; the "came from the SD package" line in Hello's asset was fixed text
(now reworded).

| Run | Result |
| --- | --- |
| 14:51 (Natartos not active) | Nothing appeared; the DJI menu looked frozen because Hello held the buttons. BACK closed Hello (exit 0) and the buttons returned. Flags cleaned up. See the debug log. |
| 14:55:19–14:55:43 (owner started Natartos at 14:54) | The overlay opened `fb_shared` and showed Hello. Joystick navigation matched DJI's own menu directions (`nat_joystick` mapping confirmed); four 5D presses registered; BACK closed Hello (exit 0) and all flags were removed. |

After the second run the overlay's in-process repaint failed (`repaint:
woke=0`); the owner heard about five beeps, consistent with the overlay's
injected BACK presses, and the screen went dark while the overlay's
fallback restarted `dji_glasses` (14:55:49; Natartos active again by
14:55:59). The owner confirmed a normal display afterwards. What was on
screen before Hello appeared is unknown (Hello was already showing when the
owner put the goggles on). A Doom tap from the More menu during the 14:42
session had exited with `repaint: woke=1`.

Device writes: the runner and bench package replaced, one new cache entry,
two Hello launch records (five in total), and the transient overlay files,
which were removed. The local adb server was stopped after each step.

## SD access with a Natartos session

At 15:06, with the 14:54 Natartos session still running, read-only checks
showed:

- `/sys/fs/selinux/enforce` is `0` (permissive), as Toolya's Natartos start
  intends. `getenforce` itself is still refused in the `su` context.
- Under Natartos, `dji_glasses` runs as `u:r:su:s0` with the same uid,
  groups and capabilities as the ADB root shell, so ADB tests represent what
  a menu-launched runner gets. Stock `dji_glasses` is not visible from `su`.
- The card is reachable only at `/mnt/media_rw/sdcard0` (exFAT, label
  `exfat`, owner `media_rw`, mode 0770); there are no `/storage` or
  `/mnt/runtime` views.
- Listing the card root now succeeds (file names were not recorded), and
  `sd_app_runner menu-list` discovers the card and reports that
  `natartos/apps` does not exist yet.

So the earlier denials coincided with SELinux enforcing, and the card is
readable once Natartos has made SELinux permissive. Whether enforcing mode
denies the `exfat` label or root's permission override was not separated.

One unintended write: a leftover redirection in the probe script created an
empty file `/mnt/media_rw/sdcard0.err` (tmpfs, not the card). It was
confirmed empty and removed at once.

Still open: launching a package from the card (no `natartos/apps` on it
yet), a clean (no-restart) return when launched from a menu entry, and
Stage B (menu entries).
