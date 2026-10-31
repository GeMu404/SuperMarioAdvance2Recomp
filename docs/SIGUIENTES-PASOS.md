# Widescreen con alto de SNES: estado y siguientes pasos

Rama `widescreen-snes` en este fork y en el fork de `gbarecomp` (submódulo).
Objetivo: vista de **alto SNES (224)** y **ancho derivado 16:9 (≈398)**. El widescreen
solo debe cambiar la vista/cámara y el rango de spawn del propio juego; nada de
lógica por objeto.

## Estado actual (WIP, tal cual quedó)
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

## Plan
1. **Fuente única** en `gbarecomp/src/runtime/view_config.h`: `game.toml` define solo
   `view_height = 224` (y opcional `aspect = 16:9`). Derivar
   `view_w = round(view_h * 16 / 9)` (398), `extra_lr = view_w - 240`,
   `extra_top = extra_bottom = (view_h - 160) / 2` (32/32). Quitar todo 430/80/16.
2. Reemplazar 240/160 literales por constantes nativas y la geometría derivada;
   unificar `kLevelObj` en un header común del juego.
3. Cámara: clamp a `room_w - view_w` en todos los módulos.
4. Spawn: **un solo** ajuste que amplía el bound de spawn/despawn del juego (tabla ED6,
   keep-right, `0x12F`) a partir de `extra_lr`. Nada de listas por objeto.
5. **Borrar**: `smw_host_layers.*`, `smw_ws_spawn_other_edge`, sistema de notas Y/fold,
   hack de Yoshi-1, `dump_*` de debug. Estimado: −1300 a −1500 líneas.
6. Con 224 de alto revisar si siguen apareciendo sprites con Y ≥ 160 y, si es así,
   resolverlo en el render (offset de vista) y no por slot.
7. Integrar en `game.toml` (revisando) las 5 funciones de
   `recomp_master_misses_AA2E.toml.frag` (`0x080307FA` ×4830, `0x0803095C`,
   `0x08030BC8`, `0x08030DC8`, `0x08031794`; todas thumb).
8. Recompilar (el binario de `build/` es del 15-oct) y probar niveles con
   `tools/sma2-boot-to-level.keys`.

## Reglas
- No subir ROM, BIOS, saves ni builds (ya están en `.gitignore`).
- Commits chicos por paso; cambios del framework en `gbarecomp`, los del juego aquí.
