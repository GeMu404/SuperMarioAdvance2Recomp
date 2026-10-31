#!/usr/bin/env python3
"""Restore the game's own 'park offscreen X' attr1 writes in generated/.

SMA2's Yoshi-family draw routines (0x0802CAxx-0x0802D1xx) hide an entry that
fails its on-screen test by storing X = 0x110 (272) or 0x130 (304): offscreen
on the 240-wide GBA. Those strh had been hand-removed in generated/ (272/304
is visible in the wide view), which left the real X masked to 9 bits in OAM,
so an offscreen Yoshi wrapped back in as a ghost. This puts the write back,
with the park X supplied by smw_ws_obj_park_x() (a spot in the 9-bit band the
wide view never shows). Idempotent. Usage:
  python tools/restore_obj_park_writes.py [--dry] [--backup-dir DIR]
"""
import glob, os, re, shutil, sys

INS = re.compile(r'^    /\* ([0-9A-F]{8})  [0-9a-f]{8} [TA] (.*?) \*/')
STRH = re.compile(r'^strh (r\d+),\[(r\d+),#0x([0-9a-f]+)\]$')

def process(path, dry):
    L = open(path).read().split('\n')
    seq = [(i, m.group(1), m.group(2)) for i, l in enumerate(L)
           for m in [INS.match(l)] if m]
    edits = []
    for k, (i, a, t) in enumerate(seq):
        m = STRH.match(t)
        if not m or m.group(3) not in ('2', 'a'): continue
        e = seq[k + 1][0] if k + 1 < len(seq) else len(L)
        blk = L[i:e]
        if any('bus_write' in x for x in blk): continue
        tr = next((x for x in range(i, e) if 'runtime_trace_event(RUNTIME_TRACE_MEM_WRITE' in L[x]), None)
        if tr is None: continue
        # The park: orrs rV,rV,r1 after movs r1,#0x88/#0x98; lsl #1.
        prev = [s[2] for s in seq[max(0, k - 8):k]]
        if not any(re.match(r'movs r1,#0x(88|98)$', p) for p in prev): continue
        v = m.group(1)[1:]
        edits.append((tr, a, v))
    if edits and not dry:
        for tr, a, v in sorted(edits, reverse=True):
            L[tr + 1:tr + 1] = [
                '    extern uint32_t smw_ws_obj_park_x(void);',
                '    bus_write_u16(_ea_%s & ~1u, (uint16_t)((g_cpu.R[%s] & 0xFE00u) | smw_ws_obj_park_x()));' % (a, v)]
        open(path, 'w').write('\n'.join(L))
    return [a for _, a, _ in edits]

dry = '--dry' in sys.argv
bdir = sys.argv[sys.argv.index('--backup-dir') + 1] if '--backup-dir' in sys.argv else None
n = 0
for f in sorted(glob.glob('generated/recompiled_*.cpp')):
    found = process(f, True)
    if not found: continue
    if not dry:
        if bdir:
            os.makedirs(bdir, exist_ok=True)
            shutil.copy2(f, os.path.join(bdir, os.path.basename(f) + '.bak-widen'))
        process(f, False)
    print(f, ' '.join(found)); n += len(found)
print('sites', n)
