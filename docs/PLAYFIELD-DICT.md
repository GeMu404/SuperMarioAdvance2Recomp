# SMA2 playfield — diccionario

Documentación operativa (no glosario vacío). Cada entrada: **qué es**,
**valores**, **límites**, **sobre qué actúa**, **dónde se toca**.

Convenciones:

- Direcciones guest: ROM GBA `08xxxxxx` / RAM `03xxxxxx` salvo nota.
- Host: `smw_extended_view.*`, `smw_sprite_cull.*`, patches en
  `recompiled_*`.
- `el` / `er` = `g_ws_extra_left` / `g_ws_extra_right` (px). Stock: 0.
  Actual típico: **79 / 79** → `view_w = 398`.
- `view_w = 240 + el + er`. `view_h = 160 + et + eb` (tall).

Companion: [`WIDESCREEN-PLAN.md`](WIDESCREEN-PLAN.md),
[`PLAYFIELD-TESTS.md`](PLAYFIELD-TESTS.md),
[`CAMERA-PLAYFIELD.md`](CAMERA-PLAYFIELD.md).

---

## A. Viewport / host

### A1. `g_ws_extra_left` / `g_ws_extra_right` (`el` / `er`)

| Campo | Valor |
|-------|--------|
| Tipo | `int` host |
| Default stock | `0` / `0` |
| Actual | `79` / `79` |
| Actúa sobre | Ancho del FB blitteado, bias, cull, spawn tables |
| Archivo | `smw_extended_view.cpp` (+ cull) |

Simétrico como `!extra_columns` SNES. **No** mezclar con deadzone Mario.

### A2. `g_ws_extra_top` / `g_ws_extra_bottom` (`et` / `eb`)

Tall bottom-anchored: expanden altura; HUD deferred. Peek vertical
separado del horizontal.

### A3. `view_w` / `view_h`

```
view_w = 240 + el + er
view_h = 160 + et + eb
```

Límites lógicos de “pantalla ancha”. Todo xmax de sprites debe usar
`view_w` (o `el+er` donde el guest ya suma 240), **no** solo `er`.

### A4. Layer1 X (cámara guest)

| Campo | Valor |
|-------|--------|
| RAM | `0x03002C04` (u16, tip. `*(u16*)`) |
| Rango stock | `0 … screens*256 - 240` (+ bias nivel) |
| Rango wide deseado | `0 … room_w - view_w` (clamp host/patch) |
| Actúa | Origen del mundo → screen; spawn; BG scroll |

### A5. `origin_x` / `bias_x` (host)

```
origin_x = clamp(L1 - el, 0, room_w - view_w)
bias_x   = L1 - origin_x
```

| Situación | `bias_x` típico | Efecto |
|-----------|-----------------|--------|
| Mid-level, L1≥el | `el` | `FB_x = guest_sx` (centrado “correcto”) |
| Start L1=0 | `0` | Blit `+el` sin mundo → **basura izq** |
| Near right clamp | varía | Peek derecho depende de max L1 |

### A6. Transform sprite → FB

```
FB_sprite_x = guest_sx - bias_x + el
```

Con `bias==el`: `FB = guest_sx`. Si se ensancha deadzone a `el+0x4E`,
Mario queda cerca de `view_w/2` en FB **pero** el feel deja de ser stock.

### A7. BG / Layer scroll host

Mismo `origin`/`bias` que sprites. Stream de tiles debe cubrir
`[origin, origin+view_w)`. Fallos → bloques `!` cortados (P6).

### A8. Layer1 wide floor (`min_l1`) — P0

| Campo | Valor |
|-------|--------|
| Fórmula | `min_l1 = min(cam_min + el, max_l1)` |
| `cam_min` | `level+0x28` (stock 0 u 8) |
| Dónde | `apply_wide_guest_cam` (después del follow), `smw_extended_view.cpp` |
| Actúa | Impide `L1=0` al start → `bias` queda `el`, `origin=0`; no peek a mundo &lt; 0 |
| Analogía SNES | cam min = `+extra_columns` |

Con `L1 ≥ el`: `origin = L1 - el` (clamped), `bias = el` en borde izq/mid.
**No** inventar tiles a la izquierda de 0 ni bajar ED6[0].

### A9. Layer1 Y follow + clamp — tall / floor framing

No centrar Mario en Y: queda a **nivel de piso**.

| Campo | Valor |
|-------|--------|
| Umbral bajo | `sy > 120` → cam baja; settle `sy ≈ 104` |
| Umbral alto | `sy < 40` → cam sube |
| Clamp | `0 ≤ L1_Y ≤ eff_room_h − view_h` |
| `eff_room_h` | `max(header room_h, L1+160, player_y+32)` — el header YI1 (256) miente |
| Safety | nunca `L1_Y` tal que Mario quede fuera de las 160 líneas guest |
| Mario | **no** se mueve |
| Dónde | `apply_guest_cam_y_follow` post-follow |

**FAIL previo:** clamp ciego a `256−224=32` → Mario muere. No repetir.

---

## B. Deadzone / follow (cámara Mario)

### B1. `0x03001D08` — deadzone / scroll center

| Facing | Valor stock | Significado |
|--------|-------------|-------------|
| Derecha | `0x4E` (78) | Mario a la **izq** del centro 120 |
| Izquierda | `0x80` (128) | Mario un poco a la **der** del centro |

Escritor: `Sub080029E8`.  
Consumidor: `Sub08006434` (follow).

**Wide — qué NO hacer (fallido):** sticky `el+0x4E` / `el+0x80` + stomp
en `smw_ws_on_layer1_x_stale` empeoró el recentrado al caminar izq.
**Qué sí (línea SNES):** dejar 0x4E/0x80 (o remap mínimo) y mover
**límites de cámara/spawn**, no el “centro de comodidad”.

### B2. `Sub080029E8` — set deadzone por facing

| | |
|--|--|
| ROM | `080029E8` |
| Patch | `recompiled_018` (constantes 4E/80) |
| Host | `smw_ws_deadzone_face_right/left` |
| Actúa | Solo escribe `1D08` |

### B3. `Sub08006434` — camera follow

| | |
|--|--|
| ROM | `08006434` |
| Lee | Mario X, `1D08`, Layer1, bounds nivel |
| Escribe | Layer1 X; **`level_base+0x1C`** (side flag spawn) |
| Side flag | `0` → usar ED6 borde **izquierdo**; `0x0101` → borde **derecho** |

Sin follow sano, spawn dual (P4) no tiene sentido.

### B4. Bounds de sala / `level+0x28`

Bias / last-screen en struct de nivel. Clamp max Layer1 debe restar
`view_w` (o 240+el+er), no solo 240.

---

## C. Spawn columns (ED6 / EE2)

### C1. `Data08100ED6` — offsets X por lado (8 entradas)

Tabla ROM; índice desde `level+0x1C` + lógica sweep.

| Índice (concepto) | Stock approx | Wide intent |
|-------------------|--------------|-------------|
| Borde izq [0] | valor que alinea spawn ~32px **izq** del cam | **No** ampliar a −127: `scan_x<0` early-out rompe incremental |
| Borde der | +240-ish | `+ (240+er)` o `+view_w` |

Host: `smw_ws_sprite_spawn_edge_table` / writers en cull.

**Lección aprendida:** ensanchar solo la **derecha** (y sweep init);
izquierda stock o `max(stock, something≥0)` hasta entender scan.

### C2. `Data08100EE2` — companion / Y u otros

Misma familia que ED6; tocar en pareja si el patch lo exige.

### C3. `Sub08036E94` — sweep / init spawn

| | |
|--|--|
| ROM | `08036E94` |
| Cuando | Carga / resync; usa offset **0x1C=2** (init path) |
| Patches | `recompiled_023/024/034/058` |
| Host | `smw_ws_sprite_sweep_*` |

Si solo se patchéa ED6 y no el sweep, el nivel arranca con columnas
stock.

### C4. `Sub08036A9C` — spawn incremental

| | |
|--|--|
| ROM | `08036A9C` |
| Trigger | Scroll; lee side flag + ED6 |
| Fail mode | Si `scan_x < 0` → return temprano → **no spawnea** hasta L1 grande |

### C5. `smw_ws_spawn_other_edge` (host)

Fuerza evaluar el borde contrario cuando el side flag se pega a un
lado (deadzone sticky). Útil tras P1 estable; no sustituye P1.

### C6. Analogía SNES

```
; wide-snes level_sprites.asm $02A7F6
db $D0 - !extra_columns
db $00
db $20 + !extra_columns
```

Misma idea: mover la **columna de spawn** ±extra, no el deadzone Mario.

---

## D. Offscreen / despawn / vis

### D1. Ventana guest clásica

| Const | Stock | Significado |
|-------|-------|-------------|
| `0xFF00` | −256-ish / signed lo | Floor izq offscreen |
| `0x013F` | 319 | Techo der offscreen (240+margen) |
| Soft OAM | `0x012F` + `sx+32` | Cull dibujo / despawn suave |

### D2. `smw_ws_sprite_offscreen_*`

Host remaps xmin/xmax. **Regla:** xmax = f(`el+er`), no solo `er`.
Bug histórico: xmax corto → Yoshi muere a la derecha.

### D3. Soft OAM `0x012F` / pad `sx+32`

Si no se ensancha `012F` y el pad (`32+el`):

- Enemigos spawneados a la **izquierda** viven ~**3 bloques** y mueren.
- Peek derecho no dibuja.

Patches: familia `recompiled_043/018/033` (offscreen), `032` (vis pad),
`050` (dual spawn related).

### D4. OAM submit `0808C4A4`

Cull final al meter sprites en OAM. Parámetros xmax / overhang deben
conocer `view_w`. Punto **P5**.

---

## E. Proximity / AI (pendiente)

SNES: `proximity_wrap_fix.asm` — distancia Mario–sprite **16-bit**.
SMA2: si thwomps/chasers fallan solo en peek, buscar CMP/SBC 8-bit
relativos a pantalla y ensanchar o forzar 16-bit. **P7**, no antes.

---

## F. Archivos host canónicos

| Archivo | Responsabilidad |
|---------|-----------------|
| `smw_extended_view.cpp/.h` | el/er/et/eb, bias, origin, L1 hooks, (evitar stomp deadzone) |
| `smw_sprite_cull.cpp/.h` | offscreen, ED6/EE2, 013F/012F, sweep, deadzone face, vis pad, spawn_other_edge |
| `recompiled_0xx` patches | Sitios concretos ROM (ver plan § historial) |
| `tools/sma2-boot-to-level.keys` | Replay boot→nivel |
| Framedumps | `/tmp/sma2-look*` (o path actual del harness) |

---

## G. Mapa rápido síntoma → símbolo

| Síntoma | Mirar primero |
|---------|----------------|
| Basura / parallax roto al **start** | A5 bias@L1=0; **A8** cam min `+el` |
| Mario demasiado centrado / recentrado izq malo | B1/B2 deadzone; **revert** el+4E sticky |
| Yoshi muere a la **derecha** | D2 xmax = el+er |
| Enemigos ~3 bloques y desaparecen | D3 `012F` / pad |
| Solo spawn **izquierda** | C1–C5 + B3 side flag |
| `!` cortado a la derecha | A7 / P6 BG stream |
| Sin enemigos mid-scroll | C4 scan_x&lt;0; ED6[0] demasiado neg. |

---

## H. Valores de referencia (sesión actual)

Actualizar cuando cambien; no tratar como eternos.

| Símbolo | Valor |
|---------|--------|
| el / er | 79 / 79 |
| view_w × view_h | 398 × 224 (tall) |
| Deadzone stock | 0x4E / 0x80 |
| Deadzone “wide center” (fallido) | el+0x4E / el+0x80 — **no usar** hasta nuevo diseño |
| Offscreen xmax | debe cubrir view_w |
| Soft pad | 32 + el (mínimo razonable) |
