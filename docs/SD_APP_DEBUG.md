# SD app debug log

## 2026-11-16 — Root path traversal requested unnecessary read access

- **Observation:** the runner's help worked on retail build 35808, but listing
  an internal Hello package failed with `open root: Permission denied`.
- **Root cause:** `mods/sd_apps/runner.c:61` originally opened `/` with
  `O_RDONLY`; ancestor traversal also requested directory read access.
  The goggles' root session can traverse `/` without reading its contents.
- **Fix:** use `O_PATH` for root/ancestor traversal, retain `O_NOFOLLOW` on
  components, and request readable access only for the selected storage root.
  A regression using an unprivileged process and a search-only ancestor
  failed before the fix and passed afterward. Device listing, validation,
  staging, two Hello runs, and cache cleanup then succeeded.
- **Class:** filesystem-traversal-permission
- **Recently-touched?** yes — first implementation, found at the first bench check.

## 2026-11-16 — Display app started without an active overlay

- **Observation:** the first Stage A run showed nothing on screen; the DJI
  menu looked frozen until BACK closed Hello, then everything recovered.
- **Root cause:** Natartos was installed but not running (`dji_glasses` had
  restarted stock), so no overlay displayed `fb_shared`, while Hello still
  grabbed the buttons. The runner claims the screen without checking that
  the overlay is live. The pre-run check was also misled by a
  `natartos.log` left over from the earlier install session.
- **Fix:** pending. Proposed: the overlay creates an acknowledgement file
  when its inject thread starts (cleared by `inject.sh` and overlay
  cleanup), and the runner waits briefly for it before starting a display
  app, otherwise releases the screen and fails. Until then, confirm a loader
  start newer than the current `dji_glasses` before display tests.
- **Class:** missing-precondition-check
- **Recently-touched?** yes — display hand-off added the same day.

## 2026-11-16 — Overlay repaint failed after Hello exited; fell back to restart

- **Observation:** after BACK closed Hello (second Stage A run), the goggles
  beeped about five times, the screen went dark, and the DJI interface
  returned about 10 s later. Log: `repaint: woke=0`, `poke failed -> restart
  fallback`.
- **Root cause:** not established. The overlay injects BACK presses and
  waits for the DJI UI to push a frame; the beeps suggest the presses were
  delivered without causing a redraw. Hypothesis: Hello was started from
  ADB over a screen where BACK only beeps, whereas apps launched from the
  More menu (Doom earlier: `woke=1`) exit with a menu open.
- **Fix:** none yet; the existing restart fallback recovered the UI. Check
  again when launching from a menu entry (Stage B).
- **Class:** ui-repaint-after-overlay
- **Recently-touched?** no — existing overlay behaviour (`mods/overlay/mod.c`).

## 2026-11-16 — SD card unreadable from the root session

- **Observation:** every SD read from the ADB root shell (`u:r:su:s0`) failed
  with `Permission denied` (60-sample probe in the morning; a runner listing
  at about 14:50), although the shell is uid 0 with full capabilities.
- **Root cause:** SELinux enforcing outside a Natartos session. With
  Natartos started (Toolya sets permissive; `/sys/fs/selinux/enforce` = 0)
  the card root at `/mnt/media_rw/sdcard0` lists normally from the same
  context. Whether enforcing mode blocks the `exfat` label or root's
  permission override was not separated.
- **Fix:** no code change. SD launching requires an active Natartos session,
  which the menu workflow already implies; documented in
  `docs/SD_APP_RUNNER.md`.
- **Class:** environment-precondition
- **Recently-touched?** no — device policy, not runner code.

## 2026-11-16 — More menu crashed dji_glasses after the menu mod was rebuilt

- **Observation:** with the rebuilt `hook_menu.so` (Zig/Clang) and four
  injected entries, opening More crashed `dji_glasses` (SIGSEGV, fault
  address `0x2f3`, exit 139) twice; `inject.sh` respawned it each time.
  The released GCC build had worked with three entries.
- **Root cause:** `inject_items` finished with a hard-coded `vmt[58](self)`,
  meant as "Postreload". On firmware 35808 slot 58 (`+0x1d0`) is
  `MxBindableMenu_DoPreFocus(self, view)`, which passes `view` straight to
  `ViewsImage_OnGetVisible` (reads `view+0x40`). No view was passed, so `x1`
  held a leftover value: `0x2b3` in the Clang build. The release GCC build
  calls through `x1`, so `view` was the function's own address; the word at
  `+0x40` is a `ret` instruction with bit 0 clear, so DoPreFocus returned
  early. The release build worked by accident; the fourth entry was
  incidental. Postreload is slot 60 (`+0x1e0`), as in the stock
  `MxBindableAbstractMenu_Reload`. Found from the tombstone registers and
  disassembly of `libew_gls.so` and both menu builds.
- **Fix:** call `MxBindableAbstractMenu__Postreload` (EW's virtual dispatcher,
  resolved with `dlsym`) instead of a vtable index; skip it if unresolved.
  `mods/menu/RE_MENU.md` corrected.
- **Class:** abi-wrong-callee-arity
- **Recently-touched?** partly — upstream code, exposed by the rebuilt mod.

## 2026-11-17 — Guard display launches and recover killed sessions

- **Observation:** the Stage A missing-overlay failure above could start an
  invisible app that held the buttons. A host review also reproduced SIGKILL
  leaving `fb_override` and `fb_shared`, blocking the next display launch.
- **Root cause:** `display_claim` treated flag creation as readiness; cleanup
  depended on the runner returning through `display_release`.
- **Fix:** the overlay now acknowledges the specific frame inode only after its
  inject thread has found a display buffer. The runner waits before starting
  the app. Persistent runner/overlay file-lock leases detect participant death,
  protect cleanup through repaint, and allow the next launch to recover stale
  SD files without taking over legacy apps. This supersedes the pending fix in
  the 2026-11-16 missing-overlay entry.
- **Validation:** the Linux suite exercises the real overlay lease helper,
  missing readiness, stale acknowledgements, cancellation, legacy sessions,
  cross-cache ownership, SIGKILL and recovery. All 76 checks passed in x86-64
  and ARM64 QEMU guests; the production ARM64 build succeeds. These are host
  checks; the updated overlay and runner have not been installed or tested
  on the goggles in this work.
- **Class:** display-lifecycle-ownership
- **Recently-touched?** yes — the runner's new display hand-off.

## 2026-11-17 — Device validation of the display lifecycle fixes

- **Result:** installed the matched production runner and overlay on retail
  build 35808 after backing up and verifying the previous files. The missing-
  overlay check failed before app launch as intended. A deliberate runner
  SIGKILL was detected by the overlay, which removed the display files.
- **UI observation:** that ADB-started kill test used the existing repaint
  restart fallback; the owner confirmed normal controls immediately afterward.
  More-menu Hello launches before and after the kill test succeeded, and the
  owner confirmed all controls and BACK return on the final launch. See
  `docs/SD_APP_BENCH_2026-11-17.md` for the evidence boundaries.
- **Installation observation:** the first `/vendor` read-only remount was busy
  until Natartos restarted. Subsequent checks confirmed `/vendor` and `/system`
  read-only and the installed hashes matched the tested build.
