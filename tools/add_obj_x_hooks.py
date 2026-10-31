#!/usr/bin/env python3
"""Add the full-screen-X hook pair to every soft-OAM attr1 write in generated/.

Pattern (all sprite draw routines): the signed screen X is cut to 9 bits with
`lsl rD,rS,#23` (then lsr #23 / orr with size bits) and stored with
`strh rV,[rB,#0x2]` (attr1). For each such lsl followed within WINDOW
instructions by that strh, insert
    smw_ws_begin_screen_x(R[rS])   at the lsl (value before the cut)
    smw_ws_note_latched_slot(R[rB]) after the strh's bus_write_u16.
Sites already hooked are skipped, so the script is idempotent. The runtime
only accepts slots inside the soft OAM and only uses a noted X whose low 9
bits match what reached OAM, so a false match is harmless.
Usage: python tools/add_obj_x_hooks.py [--dry] [--backup-dir DIR]
"""
import glob, os, re, shutil, sys

WINDOW = 16
INS = re.compile(r'^    /\* ([0-9A-F]{8})  [0-9a-f]{8} [TA] (.*?) \*/')
LSL = re.compile(r'^(?:movs|lsls|mov|lsl)s? (r\d+),(r\d+)(?:,lsl)? #23$')
LSL2 = re.compile(r'^(?:movs|lsls|mov|lsl)s? (r\d+),(r\d+),lsl #23$')
STRH = re.compile(r'^strh (r\d+),\[(r\d+),#0x2\]$')

def blocks(lines):
    out = []
    for i, l in enumerate(lines):
        m = INS.match(l)
        if m: out.append([i, m.group(1), m.group(2)])
    return out

def process(path, dry):
    lines = open(path).read().split('\n')
    seq = blocks(lines)
    ins = []  # (line_index_after, text)
    added = 0
    for k, (i, addr, txt) in enumerate(seq):
        m = LSL2.match(txt) or LSL.match(txt)
        if not m: continue
        src = m.group(2)
        end = seq[k + 1][0] if k + 1 < len(seq) else len(lines)
        if any('smw_ws_begin_screen_x' in x for x in lines[i:end]): continue
        st = None
        for (j, a2, t2) in seq[k + 1:k + 1 + WINDOW]:
            if t2.startswith(('bl ', 'b ', 'bx ', 'pop', 'push')) and not STRH.match(t2):
                if t2.startswith(('bl ', 'bx ', 'pop')): break
            ms = STRH.match(t2)
            if ms: st = (j, a2, ms.group(2)); break
        if not st: continue
        rm = None
        for x in range(i, end):
            if lines[x].strip().startswith('uint32_t _rm_%s = g_cpu.R[' % addr):
                rm = x; break
        if rm is None: continue
        j, a2, base = st
        jn = next((b[0] for b in seq if b[0] > j), len(lines))
        if any('smw_ws_note_latched_slot' in x for x in lines[j:jn]):
            pass  # strh already noted; only latch is missing
            note = None
        else:
            wr = next((x for x in range(j, jn) if 'bus_write_u16(_ea_%s' % a2 in lines[x]), None)
            if wr is None: continue
            note = wr
        sreg = src[1:]
        ins.append((rm, '    extern void smw_ws_begin_screen_x(int32_t);\n'
                        '    smw_ws_begin_screen_x(static_cast<int32_t>(g_cpu.R[%s]));' % sreg))
        if note is not None:
            ins.append((note, '    extern void smw_ws_note_latched_slot(uint32_t);\n'
                              '    smw_ws_note_latched_slot(g_cpu.R[%s]);' % base[1:]))
        added += 1
    if added and not dry:
        for idx, text in sorted(set(ins), reverse=True):
            lines[idx + 1:idx + 1] = [text]
        open(path, 'w').write('\n'.join(lines))
    return added

def main():
    dry = '--dry' in sys.argv
    bdir = None
    if '--backup-dir' in sys.argv:
        bdir = sys.argv[sys.argv.index('--backup-dir') + 1]
    total = 0
    for f in sorted(glob.glob('generated/recompiled_*.cpp')):
        n = process(f, True)
        if n and not dry:
            if bdir:
                shutil.copy2(f, os.path.join(bdir, os.path.basename(f) + '.bak-hooks2'))
            process(f, False)
        if n: print(f, n)
        total += n
    print('sites', total)

main()
