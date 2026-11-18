# Decision Log

Forward-facing, append-only record of architectural and behavioral decisions.
Newer applicable entries supersede older decisions.

## 2026-11-17 — Keep the boot marker experiment separate from activation

- **Decision:** develop a getter-forwarding GPS proxy on `codex/boot-marker-probe`,
  based on upstream `master` revision `82f1a7d`. Its constructor may append one
  bounded diagnostic record to an existing marker; the stock getter's argument
  and returned interface remain unchanged. Keep it outside release packaging
  and installation flows.
- **Why:** establish execution and its actual security context before designing
  unattended launcher activation. This experiment does not start Natartos,
  change SELinux, access physical memory or provide a generic HAL replacement.
  Original-library availability and real GPS behavior need separate bench checks.
- **Affects:** `src/boot_probe/`, its authored fixtures, build/test tools and
  `docs/BOOT_MARKER_PROBE.md`. Device deployment is outside this change.

## 2026-11-18 — Separate boot qualification from guarded activation

- **Decision:** keep the marker branch intact and build optional activation on
  `codex/boot-activation`. First qualify a detached static helper with memory
  writes and GUI control compiled out. The full helper requires an explicit
  opt-in, startup bypass, exact kernel/boot-image profile, durable attempt
  record, pinned mods and an overlay frame-hook acknowledgement.
- **Why:** constructor execution alone does not establish permission-transition
  correctness or usable UI startup. The existing injector's retry and
  alive-only health behavior are insufficient for unattended startup. Use a
  bounded session supervisor and independent pipe watcher for stock fallback;
  runtime permissive mode, once entered, lasts until reboot.
- **Affects:** optional boot artifacts, loader pipe health and
  `docs/BOOT_ACTIVATION.md`. Production profiles and raw device evidence remain
  private; this does not add release installation or firmware flashing.

## 2026-11-18 — Keep boot decisions independent of input devices

- **Decision:** remove button-state checks from qualification and activation.
  Use the persistent enable/disable files, exact-build checks, attempt limits
  and automatic stock fallback. The startup delay requires no owner action.
- **Why:** unattended boot must not depend on a timed key action or input-device
  access. The first enforcing cold qualification stopped at the input gate
  before reaching its intended read-only permission probe.
- **Supersedes:** the input-based bypass in the initial boot activation design.
- **Affects:** `src/boot_activate/supervisor.c`, its build audit/tests and the
  bypass contract in `docs/BOOT_ACTIVATION.md`.

## 2026-11-18 — Make boot health independent of libc environment availability

- **Decision:** preserve the fixed stock Android GUI environment and allow the
  freestanding loader to read its initial health fields directly from procfs
  when libc lookup supplies neither. Bound the read and reject malformed input.
  A successful complete pipe report is required before marking boot health.
- **Why:** a real warm test reached the overlay hook but selected manual health
  behavior because libc lookup did not expose the boot fields.
- **Affects:** loader health setup/reporting, GUI exec environment and the
  Android-linker fixture. The render criterion and 30-second deadline stay fixed.

## 2026-11-18 — Keep boot health out of default release builds

- **Decision:** compile the loader's inherited-pipe health path only with
  `NATARTOS_BOOT_ACTIVATION`; the experimental builder opts in explicitly and
  also builds a default loader to audit that separation.
- **Why:** a default per-session release should retain its existing startup
  behavior while the boot supervisor remains an experimental contribution.
- **Affects:** loader build flag, activation cross-build audits, README entry
  and the separate boot-activation CI workflow. No deployment changes.
