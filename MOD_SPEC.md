# mod.json — mod descriptor specification

Every mod ships an `<id>.json` next to its `.so` in `/vendor/natartos/mods/`.
The file is optional — a `.so` with no json still works (legacy: the loader
loads every `*.so`).

> New to writing mods? Start from [`mods/template`](mods/template) — the smallest
> working mod, with build and deploy steps.

## Format

```json
{
  "id": "doom",
  "name": "Doom",
  "version": "1.0",
  "type": "app",
  "so": null,
  "menu": {
    "label": "Doom",
    "action": "launch"
  },
  "launch": {
    "binary": "/data/local/tmp/doom_goggles",
    "args": ["-iwad", "/data/local/tmp/freedoom1.wad"],
    "display": true
  }
}
```

## Mod types

### `app` — a launchable application

A standalone binary that draws to the goggles' display. Examples: Doom, an
emulator, a terminal.

`display: true` — the app writes to the framebuffer (via fb_shared). The menu
activates fb_override.

### `tweak` — a toggleable mod

Turned on/off at runtime. State lives in a flag file.

```json
{
  "id": "mute",
  "name": "Mute Audio",
  "type": "tweak",
  "so": "mod_mute.so",
  "menu": { "label": "Mute Audio", "action": "toggle" },
  "toggle": {
    "flag": "/data/local/tmp/natartos_mute"
  }
}
```

On toggle: `touch <flag>` / `rm <flag>`. The `.so` checks the flag at runtime.
For several related flags use `"action": "submenu"` with a `"toggles": [...]`
array (see `mods/mute/mod.json`).

### `theme` — a cosmetic mod

Replaces resources (bitmaps, fonts). Toggleable.

```json
{
  "id": "custom_bitmaps",
  "name": "Custom Bitmaps",
  "type": "theme",
  "so": "hook_bitmap.so",
  "menu": { "label": "Custom Bitmaps", "action": "toggle" },
  "toggle": {
    "flag": "/data/local/tmp/natartos_bitmap"
  }
}
```

### `hook` — a background hook (no menu)

Loaded by the loader, runs silently.

```json
{
  "id": "hook_debug",
  "name": "Display Hook",
  "type": "hook",
  "so": "hook_debug.so",
  "menu": null
}
```

## Fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `id` | string | yes | Unique mod slug (kebab-case) |
| `name` | string | yes | Display name |
| `version` | string | no | Mod version |
| `type` | enum | yes | `app` / `tweak` / `hook` / `theme` |
| `so` | string/null | no | `.so` filename (null if no code) |
| `menu` | object/null | no | null = not shown in the menu |
| `menu.label` | string | yes* | Menu item text |
| `menu.action` | enum | yes* | `launch` / `toggle` / `submenu` / `info` |
| `launch` | object | for app | Launch parameters |
| `launch.binary` | string | yes* | Path to the binary |
| `launch.args` | string[] | no | Arguments |
| `launch.display` | bool | no | true = writes to the display |
| `toggle` | object | for tweak/theme | Toggle parameters |
| `toggle.flag` | string | yes* | Path to the flag file |

(*) Required only when the parent object is present.

## Layout

```
/vendor/natartos/mods/
  ├── hook_debug.so
  ├── hook_debug.json
  ├── hook_bitmap.so
  ├── hook_bitmap.json
  ├── mod_menu.so           ← EW-menu injector (natartos_mod_init protocol)
  ├── mod_menu.json
  ├── mod_mute.so           ← audio mute toggle (natartos_mod_init protocol)
  ├── mod_mute.json
  └── mod_doom.json         ← app with no .so (binary lives in /data)
```

Naming: a mod's `.so` is named `hook_<id>.so` (a background hook) or `mod_<id>.so`
(a menu-integrated mod) — both load the same way; pick a clear name. Menu-integrated
mods also export **`natartos_mod_init`**, which the loader calls first (passing its
`dlsym`) before `natartos_mod_register` (see below) adds the menu entry.

Legacy `.so` files without a `.json` still work (the loader loads every `*.so`).

## Configuration

`tweak`/`theme` mods store state in a flag file (e.g.
`/data/local/tmp/natartos_<id>`). The mod checks for the file on every hook call —
no file = disabled.

## Adding a menu entry (optional)

A menu item is **optional** — set `"menu": null` (or omit it) and the mod runs
silently, toggled only by its flag file. If you do want an entry in the goggles'
**More** menu, your `.so` exports `natartos_mod_register`. The loader calls it on
load and passes `dlsym`, so you can reach the loader's menu API:

```c
int natartos_mod_register(void *(*dlsym_fn)(void *, const char *)) {
    natartos_menu_register_fn reg =
        (natartos_menu_register_fn)dlsym_fn((void *)0, "natartos_menu_register");
    if (reg) {
        natartos_menu_entry e = {
            .id       = "mymod",            /* unique key */
            .name     = "My Mod",           /* shown in the menu */
            .subtitle = "Toggle example",   /* second line, or NULL */
            .command  = "sh -c 'f=/data/local/tmp/natartos_mymod; "
                        "if [ -f $f ]; then rm -f $f; else touch $f; fi'",
            .priority = 300,                /* sort order: lower = higher */
        };
        reg(&e);
    }
    return 0;
}
```

`natartos_menu_entry` (from `natartos_common.h`):

| Field | Meaning |
|-------|---------|
| `id` | unique key, e.g. `"doom"` |
| `name` | display name |
| `subtitle` | second line, or `NULL` |
| `icon_path` | NC01 `.rgba` icon file, or `NULL` for the fallback |
| `command` | shell command run on tap, or `NULL` |
| `on_action` | callback run on tap (used when `command == NULL`) |
| `priority` | sort order; lower = higher in the menu |

On tap the menu runs `command` (or calls `on_action` when `command` is
`NULL`). Both run on the UI thread, so anything long-running must background
itself (`nohup ... &`, as Doom and PC Stream do). A toggle just flips its flag
file, as above. Registering an existing `id` again updates that entry. Up to
`NATARTOS_MAX_MENU_ITEMS` (32) entries are shown across all mods. Mirror the
same item in `mod.json`'s `menu` block so the descriptor matches what the code
registers.

## DJI Embedded Wizard menu integration

`mod_menu.so` patches DefineMenu (vtable slot 11, offset 0x58) on all 10
`UavMenuTree` subclasses in `libew_gls.so`, using the `natartos_mod_init`
protocol to obtain dlsym. See [`mods/menu/RE_MENU.md`](mods/menu/RE_MENU.md) for
the reverse-engineering notes.

For each mod with `menu != null`, the planned item is:
- `action: "launch"` → tap launches the application
- `action: "toggle"` → tap flips the flag file
- `action: "submenu"` → a submenu of several toggles
- `action: "info"` → name and version only (read-only)
