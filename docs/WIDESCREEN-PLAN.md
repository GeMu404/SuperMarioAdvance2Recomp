# SMA2 widescreen — plan de ataque

**Objetivo final:** que el guest SMA2 se comporte como SMW (física, cámara,
spawn/despawn, parallax) con un viewport host más ancho/alto
(`view_w = 240+el+er`, hoy **398×224** con `el=er=79`, tall bottom-anchored),
**sin** remendar el HUD con copias de FB ni pin de scanlines.

Cada chat nuevo debería atacar **un solo punto** de la lista §4, marcar la
prueba en [`PLAYFIELD-TESTS.md`](PLAYFIELD-TESTS.md) y actualizar el
diccionario si aparece un símbolo nuevo.

Índice vivo:

| Doc | Rol |
|-----|-----|
| [`CAMERA-PLAYFIELD.md`](CAMERA-PLAYFIELD.md) | SSOT corto memoria/host (histórico) |
| [`PLAYFIELD-DICT.md`](PLAYFIELD-DICT.md) | Diccionario: RAM, funcs, constantes, hooks |
| [`PLAYFIELD-TESTS.md`](PLAYFIELD-TESTS.md) | Protocolo de pruebas pass/fail |
| **este archivo** | Contexto SMW/SNES + orden de trabajo |

---

## 1. Cómo funciona la cámara en SMW (stock)

Modelo mental (SNES y SMA2 son el mismo diseño, distinto packing de RAM):

1. **Cámara = borde izquierdo del viewport nativo**  
   SNES: `$1A/$1B` (Layer1 X). SMA2: Layer1 X en `0x03002C04`.
2. **Mario no está centrado al 50%.** Hay un *deadzone* / “scroll window”:
   - Face-right: centro ≈ **0x4E (78)** → Mario a la **izquierda** del centro.
   - Face-left: centro ≈ **0x80 (128)** → Mario un poco a la **derecha**.
   - Bordes del follow = centro ± 12.
3. Steady state al caminar derecha:  
   `cam ≈ player_x - deadzone` → `screen_x ≈ deadzone`.
4. **Clamp de sala:** no scrollear pasado el último screen − viewport nativo
   (240 en GBA / 256 en SNES). En SMA2: `max ≈ screens*256 - 240` (+ bias
   `level+0x28`).
5. **Inicio de nivel:** a menudo **no** snappea Layer1 X a Mario; el follow
   alcanza después. Por eso “Mario a la izquierda, cámara quieta” un rato.

SMA2 entry points (ver dict):

- Facing → deadzone: `Sub080029E8` escribe `1D08` = `0x4E` / `0x80`.
- Follow: `Sub08006434`.
- Side flag para spawn: el follow escribe `level+0x1C` = `0` (borde izq ED6)
  o `0x0101` (borde der ED6).

---

## 2. Qué hace el patch widescreen SNES (VitorVilela7 / wide-snes)

Referencia: [wide-snes](https://github.com/VitorVilela7/wide-snes)
(`super-mario-world/asm/`).

### 2.1 Expansión del viewport

| Modo | `!extra_columns` | Ancho interno |
|------|------------------|---------------|
| normal | 48 | 352 (=256+96) |
| extra | 64 | 384 |
| ultra | 96 | 448 |
| hyper | 112 | 480 |

`!screen_size = 256 + 2*!extra_columns` (simétrico L/R).

bsnes-hd aporta “overcolumns” (PPU); el ASM asume ese ancho y **mueve
límites de juego**, no inventa FB HUD.

### 2.2 Cámara

`main.asm` hijackea límites horizontales/verticales:

- Izquierda: no bajar de `0 + extra_columns`.
- Derecha: `last_screen - extra_columns`.
- Mario no puede acercarse más de `8-extra` al borde izq ni más de
  `E8+extra` al derecho (push de pantalla).

Efecto: al **entrar** a un nivel, la cámara ya arranca “metida”
`extra_columns` px → **no se ve basura a la izquierda** ni parallax
negativo. Esto es el análogo de lo que pediste para mañana.

### 2.3 Spawn (“spawn column”)

Patreon / `level_sprites.asm`:

- Stock: columna mágica ~32 px dentro del borde.
- Wide: tabla de rango horizontal en `$02A7F6`:

```
; stock roughly D0 / 00 / 20
db $D0 - !extra_columns
db $00
db $20 + !extra_columns
```

Mueve la línea de spawn **hacia afuera** (48/64/… px) para que los
enemigos nazcan **fuera** del borde visible y entren al scrollear.

### 2.4 Despawn / offscreen / draw

- Tablas `SUB_OFF_SCREEN` ensanchadas ±`extra_columns`.
- Flags nuevos (`!sprite_wide_flag_table`) para el 9º bit de X en OAM.
- Comparaciones tipo `ADC #$40` / `CMP #$180` → suman `2*extra` al ancho.
- Generators / shooters / extended / cluster: cada familia con su ASM
  (`level_generators.asm`, etc.).

### 2.5 Proximity (AI)

`proximity_wrap_fix.asm`: distancias Mario↔sprite en **16-bit signed**
(antes 8-bit wrap en bordes). No mueve spawn; evita IA rota en el peek.

### 2.6 Lecciones para SMA2

| SNES wide | SMA2 análogo |
|-----------|--------------|
| `extra_columns` simétrico | `g_ws_extra_left` / `g_ws_extra_right` |
| Cam min = `+extra` al start | **P0 mañana:** no peek izq / o L1≥el |
| Spawn table ±extra | `Data08100ED6` + sweep `Sub08036E94` |
| Offscreen tables ±extra | `smw_ws_sprite_offscreen_*`, `0x013F`, `0x012F` |
| PPU overcolumn | Host blit + `bias` / `origin` |
| Proximity 16-bit | Pendiente (IA thwomp/etc. si falla) |

**No** copiar ciegamente `el+0x4E` como deadzone sticky: SNES **no**
recentraliza a `view_w/2`; mantiene el feel 0x4E/0x80 relativo al
viewport **nativo** y mueve los **bordes del mundo**.

---

## 3. Modelo SMA2 host (estado actual)

```
view_w = 240 + el + er          # 398
guest Layer1 = cam nativa 240
origin_x = clamp(L1 - el, 0, room_w - view_w)
bias_x   = L1 - origin_x        # 0 en borde izq; el a mitad de sala
FB_sprite_x = guest_sx - bias + el   # == guest_sx si bias==el
```

Problema de arranque: `L1=0` → `bias=0` pero el blit aún usa `+el` →
margen izquierdo con BG/parallax mal streamados.

Hooks vivos: `smw_extended_view.cpp`, `smw_sprite_cull.cpp`
(ver diccionario).

---

## 4. Orden de ataque (chats nuevos = 1 punto)

Marcar en [`PLAYFIELD-TESTS.md`](PLAYFIELD-TESTS.md) al cerrar cada chat.

| # | Punto | Por qué | Dependencias |
|---|--------|---------|--------------|
| **P0** | Arranque sin basura izq / parallax sano | Pedido explícito; base visual | ninguna |
| **P1** | Deadzone = stock feel (`0x4E`/`0x80` mapeado bien) | Recentrar izq/der como original; **revertir** el fallo actual | P0 ayuda |
| **P2** | Clamp Layer1 max = `room_w - view_w` (+bias) | Peek derecho al final del nivel | P0 |
| **P3** | Offscreen/despawn X (`FF00`, `013F`, `012F`, pad) | Yoshi/enemigos no mueren a ~3 bloques ni a la der | P1/P2 |
| **P4** | Spawn: ED6/EE2 + sweep init + ambos bordes | Spawnear izq **y** der | P3 |
| **P5** | OAM submit cull (`0808C4A4` xmax/overhang) | Sprites se dibujan en el peek | P3 |
| **P6** | BG stream / tile peek derecho | `!` blocks no cortados | P2 |
| **P7** | Proximity / AI 16-bit (si hace falta) | Thwomps etc. en peek | P4 |
| **P8** | HUD como UI (diferido) | Fuera de plan A playfield | todo lo anterior estable |

**Regla:** no mezclar P1 (cámara) y P4 (spawn) en el mismo chat. Spawn
depende del side flag `level+0x1C` que escribe el follow.

---

## 5. Cómo abrir un chat nuevo

Plantilla:

```
SMA2 widescreen — solo P# <nombre>
Repo: sma2-recomp-src
Leer: docs/WIDESCREEN-PLAN.md §4, docs/PLAYFIELD-DICT.md (<sécciones>),
      docs/PLAYFIELD-TESTS.md T-P#
No tocar otros puntos. Al terminar: actualizar TESTS (PASS/FAIL + notas)
y DICT si hay símbolos nuevos.
```

Replay útil: `tools/sma2-boot-to-level.keys` + framedump 398×224.

**P0 / cam Y (2026-09-23):** wide `min_l1=+el` OK. Tall = top-anchor
(`et=0/eb=64`), HUD arriba. Follow Y = dict **A9** (umbral piso + clamp
`L1_Y ≤ room_h − view_h`). No one-shot cam patches ni status-detach.

---

## 6. Definición de “hecho”

- Cámara: feel stock (izq/der por facing), sin basura al start, clamp wide.
- Spawn/despawn: enemigos entran por **ambos** bordes y viven en todo el
  `view_w` (no ~48 px).
- Parallax/BG: sin seams ni peek a tiles no streameados.
- Física Mario/enemigos: mismas velocidades/colisiones guest; solo cambian
  ventanas relativas a cámara.
- HUD: aún deferred; no cuenta como bloqueante de playfield.
