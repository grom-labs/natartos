# RE: Menu Injection — reference

## Two-layer architecture

| Layer | Classes | Role |
|-------|---------|------|
| **Definition** | `UavMenuTree` subclasses (UavWa520, UavZa530...) | Declares menu items (tree of definitions) |
| **Rendering** | `DuThemedControlsMoreMenu` | Draws items on screen (widgets, layout) |

Only the **rendering layer** works for injecting into More menu.

## Adding items to Settings root

Hook slot 11 (DefineMenu) on `UavMenuTree` VMTs. After `original(tree)`:
```c
void *root_menu = *(void **)((char *)tree + 0x80);
void *item = MxMenuDefinitionMenu_AddItem2(root_menu, title, id);
*(int *)((char *)item + 0x44) = 0;  // bypass data source lookup
```
Only `id=1` is safe at root level; others cause SIGSEGV.

## Adding items to More Menu (working approach)

PLT-interpose `MxBindableAbstractMenu_Reload`:
1. Filter by `*(void**)self == __vmt_DuThemedControlsMoreMenu`
2. Call original Reload, then:
   - `menudata = self[73]` (offset 0x248)
   - `md_vmt[11]` = count, `md_vmt[12]` = get_item(index)
3. Clone 256-byte template from last stock item
4. Patch: `d[4]=action, d[5]=icon, d[16]=title, d[17]=subtitle, d[18]=type(31)`
5. `MxBindableAbstractMenu_AddItemFromData(self, item_data)`
6. `CoreVerticalList_OnSetNoOfItems(self, 0)` + `MxBindableAbstractMenu__Postreload(self)`
   (EW's virtual dispatcher, resolved with `dlsym`). Do not hard-code the
   vtable slot: on build 35808 Postreload is slot 60 (`+0x1e0`) and slot 58
   (`+0x1d0`) is `MxBindableMenu_DoPreFocus(self, view)`, which crashes when
   called without a view. The stock `MxBindableAbstractMenu_Reload` itself
   ends with Prereload (`+0x1d8`) … `AddItemFromData` … Postreload (`+0x1e0`).

## VMT names

UavMenuTree subclasses (10 total):
`__vmt_UavInvalidUavMenuTree`, `__vmt_UavWa020UavMenuTree`, `__vmt_UavWa140UavMenuTree`,
`__vmt_UavWa233UavMenuTree`, `__vmt_UavWa520UavMenuTree`, `__vmt_UavWa521UavMenuTree`,
`__vmt_UavWa530UavMenuTree`, `__vmt_UavWm169unitUavMenuTree`, `__vmt_UavZa530UavMenuTree`,
`__vmt_UavZa5305UavMenuTree`

Rendering: `__vmt_DuThemedControlsMoreMenu`, `__vmt_DuThemedControlsMainMenu`

## Item data layout (unsigned long[])

| Offset | Field |
|--------|-------|
| `[4]`  | `MxBindableMenuItemSubTitleActionValue` (action object, 64B) |
| `[5]`  | `ResourcesBitmap` icon |
| `[16]` | title (XString) |
| `[17]` | accessibility subtitle (XString) |
| `[18]` | type (set to 31 for indexed-image-action) |

Action value layout (at `[4]`):
- `+0x20` (32B): slot `{void *object, void *method}` — action dispatch target
- `[6]`: subtitle XString (displayed in UI)

## Critical: do NOT export EwSignal

EwSignal PLT interposition causes SIGSEGV after 30-40s. Keep signal-related
functions `static`. Action dispatch uses marker-based slot patching on f[4]+0x20.
