# SMA2 playfield — documento de pruebas

Protocolo pass/fail para atacar de a un punto (ver
[`WIDESCREEN-PLAN.md`](WIDESCREEN-PLAN.md) §4).

**Cómo usar:** en cada chat, copiá la fila T-P# relevante, ejecutá los
pasos, marcá **PASS / FAIL / BLOCKED**, pegá 1–2 líneas de evidencia
(framedump path, nota visual, log). No marques PASS “a ojo” sin replay
o captura.

**Setup común**

```bash
# desde sma2-recomp-src (ajustar binario/harness del repo)
# view 398x224, el=er=79, tall bottom-anchored
# replay: tools/sma2-boot-to-level.keys
# framedump: p.ej. /tmp/sma2-look-<tag>.png
```

Criterio global de done: plan §6.

---

## Leyenda de resultado

| Marca | Significado |
|-------|-------------|
| PASS | Cumple criterio de aceptación |
| FAIL | Reproducible; no cumple |
| BLOCKED | No se pudo probar (build/replay/deps) |
| SKIP | Fuera de alcance de este chat a propósito |

---

## T-P0 — Arranque sin basura izquierda / parallax sano

| Campo | Contenido |
|-------|-----------|
| Punto | P0 |
| Dict | A4–A7, B4 |
| Pasos | 1) Boot → primer nivel con replay. 2) Frame 0–2s **sin** caminar. 3) Mirar franja izquierda (~el px) y Layer2/3. |
| Esperado | Sin basura/tile glitch a la izq; parallax continuo; no “hueco” negro raro. |
| Analogía SNES | Cam min = `+extra_columns` al entrar. |
| Resultado | **FAIL** (parcial: cam OK; tall seam + altura rota) |
| Evidencia | Cam: `apply_wide_guest_cam` → start `L1=79 bias=79 origin=0`; walk `L1→501 bias=79`. Dumps históricos buenos: `/tmp/sma2-look-p0/patched-{800,1600}.png` **398×224**. Estado actual tras restore: `final-{800,1600}.png` salen **398×160** (`tall_bottom_anchor` en `main.cpp` pero `runtime.cpp` ya no aplica `extra_top=64`). Costura horizontal ~y=64 (BG3 wrap en peek tall) sigue en dumps 224. |
| Notas | Cam floor = dict A8, **no revertir**. Tall seam = bug aparte (ver dead-ends abajo). **No** usar binario snapshot `--window` (314×160, parpadeo). **No** `git checkout` de `gbarecomp/` sin restaurar tall wiring. |

---

## T-P1 — Deadzone / feel de cámara stock

| Campo | Contenido |
|-------|-----------|
| Punto | P1 |
| Dict | B1–B3 |
| Pasos | 1) Caminar **derecha** 5s steady. 2) Caminar **izquierda** 5s. 3) Comparar screen_x Mario vs stock (0x4E / ~0x80), no vs centro 199. |
| Esperado | Face-R: Mario claramente a la **izq** del centro FB. Face-L: un poco a la **der**. Al revertir dirección, la cámara **sigue** (no se pega ni “pierde” el recentrado). |
| Anti-esperado | Sticky `el+0x4E` que deja Mario en el centro y rompe walk-left (regresión conocida). |
| Resultado | □ PASS □ FAIL □ BLOCKED |
| Evidencia | |
| Notas | |

---

## T-P2 — Clamp derecho Layer1 = room − view_w

| Campo | Contenido |
|-------|-----------|
| Punto | P2 |
| Dict | A4, A5, B4 |
| Pasos | 1) Correr hasta el **final** del nivel. 2) Empujar derecha. 3) Ver peek derecho y Mario vs borde. |
| Esperado | Cámara llega a mostrar los últimos `er` px de mundo; Mario no se “traba” a 240-only; sin void a la der. |
| Resultado | □ PASS □ FAIL □ BLOCKED |
| Evidencia | |
| Notas | |

---

## T-P3 — Offscreen / despawn en todo view_w

| Campo | Contenido |
|-------|-----------|
| Punto | P3 |
| Dict | D1–D3 |
| Pasos | 1) Spawn o atraer Yoshi/enemigo. 2) Llevarlo al **borde derecho** del FB. 3) Spawnear enemigo por la **izquierda** y caminar ~5–6 bloques. |
| Esperado | Yoshi/enemigo **vivo** en el borde der. Enemigo izq **sigue vivo** tras ≫3 bloques (no despawn soft a ~48px). |
| Fallos históricos | xmax solo `er` → muerte der. `012F` stock → muerte ~3 bloques. |
| Resultado | □ PASS □ FAIL □ BLOCKED |
| Evidencia | |
| Notas | |

---

## T-P4 — Spawn ambos bordes

| Campo | Contenido |
|-------|-----------|
| Punto | P4 |
| Dict | C1–C5, B3 |
| Pasos | 1) Nivel con población stock conocida. 2) Scroll **derecha** sostenido: deben entrar sprites por la der. 3) Scroll **izquierda**: entrar por la izq. 4) Contar enemigos mid-level vs expectativa stock. |
| Esperado | Entrada visible por **ambos** lados; no solo “los que ya estaban a la izq”. |
| Dependencia | P1 (side flag) + P3 (si spawnean y mueren al toque → falso FAIL de spawn). |
| Resultado | □ PASS □ FAIL □ BLOCKED |
| Evidencia | |
| Notas | |

---

## T-P5 — OAM / draw en peek

| Campo | Contenido |
|-------|-----------|
| Punto | P5 |
| Dict | D4, A6 |
| Pasos | 1) Con spawn OK, enemigo en franja `x ∈ [240, view_w)`. 2) Framedump. |
| Esperado | Sprite **dibujado** (no solo “vivo en RAM”); sin clip duro a 240. |
| Resultado | □ PASS □ FAIL □ BLOCKED |
| Evidencia | |
| Notas | |

---

## T-P6 — BG stream peek derecho

| Campo | Contenido |
|-------|-----------|
| Punto | P6 |
| Dict | A7 |
| Pasos | 1) Mid-level + final de sala. 2) Mirar tiles en `x > 240` del FB (bloques `!`, ground, pipes). |
| Esperado | Tiles completos, sin corte a 240 ni seams de stream. |
| Resultado | □ PASS □ FAIL □ BLOCKED |
| Evidencia | |
| Notas | |

---

## T-P7 — Proximity / AI en peek (si aplica)

| Campo | Contenido |
|-------|-----------|
| Punto | P7 |
| Dict | E |
| Pasos | 1) Thwomp / chaser / sprite con proximidad. 2) Activar desde el peek (fuera de 240). |
| Esperado | Misma reacción que stock a igual distancia mundo; sin wrap 8-bit raro. |
| Resultado | □ PASS □ FAIL □ SKIP |
| Evidencia | |
| Notas | |

---

## T-P8 — HUD (tall detach)

| Campo | Contenido |
|-------|-----------|
| Punto | P8 |
| Dict | A2 |
| Pasos | 1) Tall 398×224 in-level. 2) HUD debe estar en el **tope** del FB. 3) Banda peek debajo del HUD, sin ghost de status a mitad. |
| Esperado | Mx / TIME / score / item box arriba; sin barra de status a oy≈64. |
| Estado | En prueba: `smw_host_layers` punch+stamp+pin (post-latch). |
| Resultado | □ PASS □ FAIL □ BLOCKED |
| Notas | Prioridad visual > plan A “no FB HUD”. |

---

## Regresión rápida (smoke 5 min)

Correr **después** de cualquier patch, aunque el chat sea de un solo P#:

| # | Check | OK? |
|---|-------|-----|
| R1 | Boot → nivel sin crash | ✓ (replay → room 5120, frame~647+) |
| R2 | Mario camina L/R | ✓ Right 1256–1600 (L1 79→501) |
| R3 | Al menos 1 enemigo visible mid-scroll | □ (no evaluado este chat) |
| R4 | Yoshi (si aplica) no muere al borde der en 10s | □ (fuera P0) |
| R5 | Franja izq start: no peor que antes del patch | ✓ cam L1 floor el (seam@el=0 en bandas BG) |
| R6 | Dump height = 224 con tall_bottom_anchor | ⏳ re-cableado runtime (`resolve_view_vertical_margins`); pendiente verify usuario |

Si R* falla y no es el P# del chat → **revertir** o aislar antes de seguir.

---

## Dead-ends P0 / tall — **no repetir**

Intentos de esta sesión que **no sirvieron**. No reintentar sin evidencia nueva.

| Intento | Resultado | Por qué no |
|---------|-----------|------------|
| Clamp Y / `tex_y` en peek tall (`hy<0`) | FAIL | WINOUT en peek deja solo BG3; top del mapa transparente → **rayas negras**. Seam casi igual. Revertido. |
| Tratar `hy` como 0 en sample Y del peek | FAIL | Dump idéntico a baseline (`fixed2`≈`patched`); o no entra al path o no cambia píxeles. |
| Skip wrap + forzar capas por DISPCNT en peek | FAIL | Si BG3 es opaco, BG2 detrás no aporta; no arregla costura. |
| **Status detach punch/stamp** | FAIL peor | Rayas negras + HUD roto; **revertido**. |
| **Top-anchor + cam-Y delta one-shot** | FAIL | Al aparecer Mario se veía zona bajo el piso; **apagado**. |
| **Bottom-anchor rollback** | no | Usuario rechazó; seguimos top-anchor + follow Y. |
| **A9 L1_Y clamp ciego room−view** | FAIL | YI1 max=32 mata Mario |
| **A9 follow + eff_room_h + alive clamp** | ⏳ | room efectivo + umbral piso; Mario on-screen |
| Tint / instrumentar solo `paint_vertical_margins` | engaña | `dump-png` / framedump a menudo usa `ppu.render()` **latch fresco**, no el path de margins → tint no aparece en PNG. Instrumentar `render()` si se valida por dump. |
| Binario snapshot + `--window` (sin tall) | FAIL setup | Abre **314×160**, pacing raro / parpadeo. Solo `build/SuperMarioWorldRecomp` del árbol con tall. |
| `git checkout --` de `gbarecomp/src/gba/gba_ppu.cpp` (y runtime) | **rompe altura** | Borra wiring widescreen/tall. Tras restore parcial: PPU vuelve a tener `extra_top`, pero `runtime.cpp` deja de setear `extra_top=64` aunque `opts.tall_bottom_anchor=true` → dumps **398×160**. Antes de tocar más seam: **restaurar** que runtime aplique tall (header ya documenta el contrato). |
| Marcar T-P0 PASS solo con cam | incorrecto | Esperado pide también parallax/tall sano. Cam min+el ≠ P0 completo. |

**Estado congelado (2026-09-23 noche):** cam P0 OK; tall 224 perdido en binario actual; seam y=64 abierto. Siguiente paso = solo re-enganchar `tall_bottom_anchor`→`extra_top=64` en runtime (sin experimentos Y nuevos).

---

## Historial de sesiones (rellenar)

| Fecha | P# | Resultado | Commit / nota |
|-------|----|-----------|---------------|
| (pre-docs) | P3 xmax | PASS parcial | xmax el+er; Yoshi der OK |
| (pre-docs) | ED6 izq −127 | FAIL | incremental bail; revert |
| (pre-docs) | P1 el+4E sticky | FAIL | recentrado izq peor; **pausar** |
| (pre-docs) | soft 012F | FAIL conocido | ~3 bloques vida izq |
| 2026-09-23 | P0 cam min +el | PASS parcial | `min_l1=cam_min+el`; dumps 398×224 históricos OK |
| 2026-09-23 | P0 tall seam / altura | FAIL | dead-ends arriba; runtime perdió `extra_top=64` |

---

## Checklist de cierre de chat

1. Una sola fila T-P# actualizada (PASS/FAIL + evidencia).
2. Si hay símbolo nuevo → 1 entrada en [`PLAYFIELD-DICT.md`](PLAYFIELD-DICT.md).
3. Si cambia el orden de ataque → 1 línea en plan §4.
4. No “casi PASS”: o cumple el **Esperado** o es FAIL.
