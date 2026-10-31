# SMA2 camera + playfield (SSOT)

Single guest system. Host widescreen/tall must derive from this — not from
ad-hoc OAM culls or HUD pin.

**Ataque widescreen (plan / dict / tests):**
[`WIDESCREEN-PLAN.md`](WIDESCREEN-PLAN.md) ·
[`PLAYFIELD-DICT.md`](PLAYFIELD-DICT.md) ·
[`PLAYFIELD-TESTS.md`](PLAYFIELD-TESTS.md)

## RAM

| Name | Addr | Notes |
|------|------|--------|
| Layer1 X | `0x03002C04` = `0x03002340+0x08C4` | **u16/s16** (`strh`) |
| Layer1 Y | `0x03002C08` = `+0x08C8` | **u16/s16** |
| Level ptr | `0x03003F98` = `base+0x1C58` | |
| Player X/Y | `base+0x1C90` / `+0x1C94` (`0x03003FD0/FD4`) | s16 world |
| Deadzone center | `base+0x1D08` | **0x4E or 0x80**, not 120 |
| Deadzone edges | `+0x1D0A` / `+0x1D0C` | center ± 12 |
| Screens raw | `level+0x1174` | header count + 1 |
| Screens X | `level+0x1175` | horiz levels: copy of 1174 |
| Screens Y | `level+0x1176` | horiz: 1; vert: copy of 1174 |
| Cam min bias | `level+0x28` | 0 or 8 |
| Follow enable | `level+0x1146` | 0 = no X follow |

## Per-frame: `Sub08006434` (`0x08006434`)

Called from `NormalGameplay`. X follow (when `1146≠0`):

```
side = (player_x - cam_x >= 1D08) ? 1 : 0
delta = player_x - cam_x - edge[side]   // edges = center±12
if delta scrolls that way: rate-limit (Sub08006398), cam_x += delta
```

Steady state: `cam_x ≈ player_x - 1D08` with `1D08 ∈ {78, 128}`.

## Room clamp (horizontal)

```
screens = u8(level+0x1175)
max_cam_x = (screens - 1) * 256
if max_cam_x != 0: max_cam_x += 0x10   // ≡ screens*256 - 240
if cam_x > max: cam_x = max + level+0x28
if cam_x < 0:   cam_x = level+0x28
```

Native view width implied: **240**. Wide must use `max = room_w - view_w`.

## Level start (`Sub0801C7E8`)

Sets player + Layer1 **Y**. Does **not** snap Layer1 X to Mario.
Centering is only the deadzone follow afterward — explains “Mario left, cam
still right” until scroll catches up.

## Host rules (derived)

```
room_w = screens_x(1175) * 256
room_h = screens_y(1176) * 256
view_w = 240 + extra_left + extra_right
view_h = 160 + extra_top + extra_bottom

// Guest cam stays native (Layer1). Host widens around it:
origin_x = clamp(layer1_x - extra_left, 0, max(0, room_w - view_w))
bias_x   = layer1_x - origin_x

// Spawn / despawn windows are relative to the SAME view (Data08100ED6/EE2):
// Do NOT widen ED6[0]/EE2[0] left — Sub0803635C bails when (L1+off)<0, so
// extra_left on the left edge kills incremental spawn until the cam is deep.
load_x_left   = -48                 // stock; sweep covers left via Layer1 walk
load_x_right  = 288 + el + er       // was 240+48
load_y_top    = -48 - extra_top
load_y_bot    = 208 + extra_bottom  // was 160+48
far_x_right   = 336 + el + er
offscreen_x   = [-16 - extra_left, 239 + extra_left + extra_right]
spawn_sweep   = back 0x60+el, last index covers view_w+48
```

Spawn / despawn already tied to `extra_*` (see `smw_sprite_cull.cpp`).

## Live check (plan A host, 398×224)

Replay `/tmp/sma2-boot.keys` + EEPROM save → Yoshi Island-size level:

| Frame | L1 X | player screen X | bias | room | deadzone |
|-------|------|-----------------|------|------|----------|
| ~750 | 0 | 86 | 0 | 5120×256 | 130 |
| ~810 | 44 | 116 | 44 | same | 102 |
| ~840+ | ≥89 | ~90 | **79** | same | **78** |
| ~1500 | scrolls | ~91 | 79 | same | 78 |

- `hdr=20/20/1` → room from **1175/1176** (not 1174).
- Left edge: bias 0 (no left peek past world 0). Mid-scroll: bias =
  `extra_left` = 79. Matches `origin = clamp(L1 - extra_l, 0, room_w - view_w)`.
- Deadzone settles at **78** (`0x4E`), Mario stays ~90px from Layer1 left.
- `origin_y` clamps to `room_h - view_h` when L1 Y fits; `bias_y` mirrors X
  when `stock_y_in_room`. Guest Y follow: dict **A9** —
  `0 ≤ L1_Y ≤ room_h - view_h`, umbral de piso (no centro).

## Wide guest follow (host bias model)

Layer1 stays a 240-wide cam; host peeks with `bias ≈ extra_left`.
Sprite FB x = `guest_sx - bias + extra_left` (equals `guest_sx` mid-scroll).

| Knob | Stock | Wide (398, el=er=79) |
|------|-------|----------------------|
| `1D08` deadzone | 0x4E face-R / 0x80 face-L | **el+0x4E / el+0x80** (facing; no sticky stomp) |
| Steady `screen_x` | ~78 / ~128 | **~157 / ~207** |
| Soft-OAM hide | sx+32 > 0x12F | **pad 32+el, thr 0x12F+el+er** |
| Spawn per frame | one ED6 edge via `level+0x1C` | **both edges** |
| Layer1 max | `room_w − 240` | `room_w − 240 − extra_right` |
| offscreen xmax | 239 | **239+el+er** |
| spawn sweep | 0x60 × 0x20 | **back+el, more cols** |

Applied in `apply_wide_guest_cam` + `smw_sprite_cull.cpp`.
