# Widescreen con alto de SNES: estado y siguientes pasos

Rama `widescreen-snes` en este fork y en el fork de `gbarecomp` (submódulo).
Objetivo: vista de **alto SNES (224)** y **ancho derivado 16:9 (≈398)**. El widescreen
solo debe cambiar la vista/cámara y el rango de spawn del propio juego; nada de
lógica por objeto.

## Estado anterior (WIP 851ffd1)
- La vista real es **430×256**, no 398×224: `src/main.cpp` usa `view_extra_top = 80`,
  `view_extra_bottom = 16` y 430 fijo en `max_view_width`, `max_resize_view_width`,
  `widescreen_view_width`; `game.toml` también tiene `view_width = 430`.
- Ningún lado calcula el ancho desde el alto (`resize_driven_view_width` en
  `gbarecomp/src/runtime/view_config.h` mantiene 160 de alto).
- Síntomas: basura en pantalla y sprites duplicados.

## Causas encontradas
1. **Wrap del BG**: anillo de 32 columnas × 16 px = 512 px (`smw_extended_view.cpp:18`)
   más el adelanto de streaming deja columnas visibles cerca de la vuelta → tiles viejos.
   En vertical, 256 de vista contra BG de 256 no deja margen (`gba_ppu.cpp:1212-1270`).
2. **Y de OAM en 8 bits**: con más de 160 de alto, sprites con Y ≥ 160 se leen como
   negativos; de ahí ~500 líneas de parches "fold"/notas Y en `smw_sprite_cull.cpp`
   (72-123, 339, 370, 390-397, 1019-1021) con estado por slot entre frames.
3. **Duplicados**: `smw_ws_spawn_other_edge` (`smw_sprite_cull.cpp:355-500`) fuerza
   spawns en todas las columnas ignorando el flag de carga del juego.
4. **Bordes inconsistentes**: `smw_sprite_cull.cpp:214` y `smw_extended_view.cpp:571`
   usan `room_w - 240` en vez de `room_w - view_w`.
5. **Código muerto**: `smw_host_layers.*` (solo con `GBARECOMP_SMW_HOST_LAYERS`),
   pero su captura se instala siempre (`smw_extended_view.cpp:798`).
6. Hardcodeos: `0x03003F98` duplicado en 3 archivos con nombres distintos; 240/160
   literales en `smw_extended_view.cpp` y `smw_sprite_cull.cpp`; caso especial Yoshi-1
   (`smw_extended_view.cpp:208-243, 577-583`).

## Hecho (2026-10-31)
1. **Fuente única**: `gbarecomp/src/runtime/view_config.h` (`derive_view_from_height`,
   `read_config_view_height`). `game.toml [video] view_height = 224`, `aspect = "16:9"`
   → 398×224, márgenes 32/32. `run_game` deriva `max_view_width`, `widescreen_view_width`
   y `view_extra_*` antes del launcher. Sin 430/80/16 en `main.cpp`/`game.toml`.
   Con el mod/widescreen apagado se queda en 240×160 nativo.
2. `src/smw_view.h`: constantes nativas (240/160 desde view_config), `view_w()/view_h()`
   y direcciones de guest (`kLevelPtr` 0x03003F98, Layer1 X/Y, soft OAM).
3. Cámara: `smw_ws_cam_span_max` ya daba `room_w - view_w`; se quitó el
   `native_max_x` muerto de `smw_extended_view.cpp`.
4. Spawn: queda solo el ensanche de las propias cotas del juego en
   `widen_load_table` (ED6, keep-right, `0x12F`), todo a partir de `extra_left+right`.
5. Borrado: `smw_host_layers.*`, el walk de spawn forzado (`smw_ws_spawn_other_edge`
   ahora es no-op), notas Y/X por slot, fold/arm/keep, hooks de mem-write y
   fn-entry de sprite_cull, `apply_guest_cam_y_follow` (código muerto) y `dump_*`.
   ~−1600 líneas. La X de OAM se resuelve con un umbral de pliegue (`wide_obj_x`)
   en el centro de la banda no usada; la Y usa el pliegue por bandas del PPU
   (`256-extra_top` / `160+extra_bottom`), sin ambigüedad con 32/32.
6. Las 5 funciones del `.frag` agregadas a `game.toml` como `[[extra_func]]` thumb.

## Pendiente / dudas (para Cursor)
- **`generated/` está parcheado a mano** (no versionado, sin generador en el repo):
  contiene miles de llamadas `smw_ws_*`. Por eso las funciones borradas quedaron como
  stubs no-op para que enlace. **No se regeneró**: `tools/regen.ps1` borraría todos esos
  hooks (incluidos `smw_ws_stream_lead`, deadzone, cotas de cull) y rompería el
  widescreen. Las 5 `extra_func` nuevas solo tendrán efecto tras regenerar; antes hay
  que convertir los parches de `generated/` en algo reproducible (hooks por
  bus-read/fn-entry o reglas del recompilador).
- El guard `stock_y_in_room` en `publish_camera` (antes "hack Yoshi-1") se dejó: es
  genérico (no clampa Y si el header del cuarto es menor que Layer1+160).
- `gba_ppu`: `g_ws_host_obj_compose` y `g_ws_obj_y_provider/attr_x_provider` quedan
  sin uso en el framework; se pueden borrar.
- Falta **probar** (no se ejecutó nada): niveles horizontales y verticales,
  sprites en las bandas de arriba/abajo, muerte de Mario (caída), item box/HUD,
  y que no haya duplicados ni basura en los bordes del anillo de BG (512 px).
- Si sigue habiendo basura de BG en los bordes, revisar `smw_ws_stream_lead` con 398.

## Reglas
- No subir ROM, BIOS, saves ni builds (ya están en `.gitignore`).
- Commits chicos por paso; cambios del framework en `gbarecomp`, los del juego aquí.

## Hecho (31-oct, tarde)

- **Cámara Y:** el máximo Y del juego (`0x03002BA8`, rutina `0x08006340`) se lee con `smw_ws_cam_y_max()` = max − `extra_bottom`. Así el borde inferior de la vista ancha queda en el fondo del mundo, igual que `smw_ws_cam_span_max` en X. Los hooks están en `generated/recompiled_040.cpp`, en `0x0800634C` y `0x08006356`. Ojo: `generated/` no se versiona.
- **Fondo mode-0 (Donut Plains 2):** el relleno de la segunda pantalla del fondo solo corre si, al decodificar las 16 columnas que cargó el juego, el resultado coincide con la VRAM. Así se autovalida por nivel y no hay números de nivel fijos. Antes pintaba tiles del primer plano sobre todos los fondos normales.
- **X de sprites:** Layer1 es el borde izquierdo de la vista (ox = 0), así que la vista es X ∈ [0, view_w). Ahora `wide_obj_x` (el doblez de 9 bits), `smw_ws_sprite_offscreen_x/_u8`, `smw_ws_wide_edge` y el pad del soft-OAM usan la misma franja, `obj_x_band() = (512 − view_w)/2`.

## Pendiente: sprites fantasma (arreglo de raíz, NO caso por caso)

Síntomas: sprites lejanos que reaparecen al otro borde (X de 9 bits que da la vuelta) y sprites que quedan pegados donde estaban (por ejemplo, Yoshi que salió de un bloque). Ya pasaba con el parche original de GeMu.

Causa: la vista mide 398 px y al anillo de 512 solo le sobran 114 px, así que un X de 9 bits es ambiguo. Cualquier ruta de dibujo del juego que no recorte justo produce un fantasma.

Plan (un solo punto, sin listas por objeto):
1. Tabla `real_x[128]` y `written_frame[128]` por slot de OAM.
2. Donde el juego escribe attr1 (X) en el soft-OAM (`smw_view::kSoftOam`), anotar la X completa con signo de ese cálculo, antes del `& 0x1FF`. Primero hay que identificar esas escrituras: el generated ya tiene hooks `smw_ws_note_obj_x` (6 sitios) y `smw_ws_note_latched_slot` (320), hoy vacíos. Partir revisando si cubren todas las rutas.
3. En la PPU, `g_ws_obj_x_provider` / `g_ws_obj_attr_x_provider` devuelven `real_x[idx]` solo si el slot se escribió en este cuadro. Si no se escribió, no se dibuja (eso mata los pegados).
4. Mismo esquema para Y (8 bits) si aparecen fantasmas verticales.
5. Una vez hecho, se pueden simplificar los recortes de arriba.
