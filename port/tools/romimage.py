#!/usr/bin/env python3
"""Generate the host side of the game's ROM-resident data (PS Vita / GCC port).

The decomp's C sources define code and some data; the rest of the game's
initialized globals (tables, strings, vtables-in-data, pointer arrays) exist
only as bytes in the cartridge's ARM9 and overlay images. On the DS the hybrid
build links those bytes in by address. A host binary links by NAME, so every
such global needs (1) host storage under its symbol name and (2) its bytes,
with every embedded DS pointer rewritten to the host address of its target.

This tool does (1) and the bookkeeping for (2) from the repo's own maps --
config/arm9/**/{symbols,delinks,relocs}.txt -- and NO ROM BYTES: the bytes are
read from the user's own dump at startup by port/hal/romimage_rt.cpp, using the
tables emitted here. Nothing derived from the ROM's contents lands in the tree.

Inputs
  --config DIR     config/arm9
  --defined FILE   symbol names the compiled objects already define (nm output,
                   one name per line). Those keep their C definitions; any
                   other name at the same DS address is aliased to them.
  --out-cpp FILE   generated storage + tables (C++)
  --out-ld FILE    linker script fragment with the aliases (NAME = OTHER;)

Output model
  For every data/rodata/bss symbol of every module (main, itcm, dtcm, ovNNN)
  that no object defines:
    * storage: one aligned byte array per distinct DS address, sized to the
      distance to the next symbol in its section (bss(size=) when given), named
      by the symbol through an asm label; other names at that address alias it;
    * if the section is .data/.rodata: a FILL entry (module, DS address, size,
      storage) -- bytes copied from that module's image at startup;
    * every `kind:load` relocation whose source lies inside a filled symbol: a
      PATCH entry (storage, offset, target), the target being the host address
      of the DS symbol containing the relocation's destination plus the
      remaining offset.
  .bss storage is left zero (the DS boot state).
"""
import argparse
import bisect
import collections
import os
import re
import sys

# Tables of numbers in which the relocation map lists words that only look
# like addresses. FX_SinCosTable_ (0x02082214, 4096 {sin, cos} s16 pairs): the
# pairs at angles 0x3ac0/0x3ad0 and 0xc530/0xc540 read as 0x020e0fde,
# 0x02080fdf, 0x0208f021, 0x020ef022, were patched into host pointers, and
# every rotation through those angles -- a bone's, an actor's facing -- came
# out about eight times too big: the one-frame giant Goombas, Koopas and
# Yoshis.
NUMERIC_TABLES = [
    ('main', 0x02082214, 0x02086214, 'FX_SinCosTable_'),
]

SECTION_RE = re.compile(r'^\s+(\.[\w.]+)\s+start:(0x[0-9a-f]+)\s+end:(0x[0-9a-f]+)\s+kind:(\w+)')
SYM_RE = re.compile(r'^(\S+)\s+kind:(\w+)(?:\(([^)]*)\))?\s+addr:(0x[0-9a-f]+)')
RELOC_RE = re.compile(r'^from:(0x[0-9a-f]+)\s+kind:(\w+)\s+to:(0x[0-9a-f]+)\s+module:(\w+)(?:\(([^)]*)\))?')

DATA_SECTIONS = ('.data', '.rodata')
STORAGE_SECTIONS = ('.data', '.rodata', '.bss')


class Module:
    def __init__(self, name, path):
        self.name = name          # 'main', 'itcm', 'dtcm', 'ov002', ...
        self.path = path
        self.sections = []        # (name, start, end, kind)
        self.syms = []            # (addr, name, kind, arg)
        self.relocs = []          # (from, kind, to, module, arg)

    @property
    def ov_id(self):
        return int(self.name[2:]) if self.name.startswith('ov') else -1

    def section_of(self, addr):
        for s in self.sections:
            if s[1] <= addr < s[2]:
                return s
        return None


def load_modules(config):
    mods = {}
    def load(name, d):
        m = Module(name, d)
        for l in open(os.path.join(d, 'delinks.txt')):
            mm = SECTION_RE.match(l)
            if not mm:
                if l.strip() and not l.startswith(' '):
                    break           # first per-file block: header done
                continue
            m.sections.append((mm.group(1), int(mm.group(2), 16), int(mm.group(3), 16), mm.group(4)))
        for l in open(os.path.join(d, 'symbols.txt')):
            mm = SYM_RE.match(l)
            if mm:
                m.syms.append((int(mm.group(4), 16), mm.group(1), mm.group(2), mm.group(3) or ''))
        for l in open(os.path.join(d, 'relocs.txt')):
            mm = RELOC_RE.match(l)
            if mm:
                m.relocs.append((int(mm.group(1), 16), mm.group(2), int(mm.group(3), 16),
                                 mm.group(4), mm.group(5) or ''))
        m.syms.sort()
        mods[name] = m
    load('main', config)
    for extra in ('itcm', 'dtcm'):
        if os.path.isdir(os.path.join(config, extra)):
            load(extra, os.path.join(config, extra))
    ovdir = os.path.join(config, 'overlays')
    for n in sorted(os.listdir(ovdir)):
        load(n, os.path.join(ovdir, n))
    return mods


def c_ident(i):
    return 'rim_%d' % i


VALID_NAME = re.compile(r'^[A-Za-z_][A-Za-z0-9_.$]*$')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--config', required=True)
    ap.add_argument('--defined', help='file of names the objects define (one per line)')
    ap.add_argument('--nm', help='nm to run over --objects instead of --defined')
    ap.add_argument('--objects', nargs='*', default=[], help='objects/archives for --nm')
    ap.add_argument('--out-cpp', required=True)
    ap.add_argument('--out-ld', required=True)
    ap.add_argument('--manifests', help='config/tu_manifest.d: undefined_symbol_aliases')
    ap.add_argument('--sources', help='slice manifest of the game TUs (for --out-init-ld)')
    ap.add_argument('--repo', help='repo root the --sources paths are relative to')
    ap.add_argument('--out-init-ld', help='linker script grouping each module\'s static initializers')
    a = ap.parse_args()

    if a.defined:
        defined = set(l.strip() for l in open(a.defined) if l.strip())
    else:
        import subprocess
        defined = set()
        objs = [o for o in a.objects if o and os.path.exists(o)]
        for i in range(0, len(objs), 200):
            outp = subprocess.run([a.nm, '--defined-only'] + objs[i:i + 200],
                                  capture_output=True, text=True).stdout
            for l in outp.splitlines():
                p = l.split()
                if len(p) >= 3 and p[1] != 'U':
                    defined.add(p[2])
    mods = load_modules(a.config)

    # ---- every symbol, per module, with its extent --------------------------
    # addr -> names, per module; and the storage decision per (module, addr).
    storages = []           # dicts: mod, addr, size, section, names
    by_key = {}             # (mod, addr) -> storage index, or 'C:<name>' if defined
    aliases = []            # (alias_name, target_name)
    for m in mods.values():
        groups = collections.OrderedDict()
        for addr, name, kind, arg in m.syms:
            groups.setdefault(addr, []).append((name, kind, arg))
        addrs = list(groups)
        for i, addr in enumerate(addrs):
            sec = m.section_of(addr)
            # sections can carry suffixes (.rodata.coin_tables): match the stem
            if not sec or not sec[0].split('.')[1].startswith(('data', 'rodata', 'bss')):
                continue
            names = [n for n, k, _ in groups[addr] if k in ('data', 'bss')]
            if not names:
                continue
            # extent: to the next symbol of the same section, else section end
            nxt = sec[2]
            for j in range(i + 1, len(addrs)):
                if addrs[j] >= sec[2]:
                    break
                nxt = addrs[j]
                break
            size = nxt - addr
            for n, k, arg in groups[addr]:
                mm = re.match(r'size=(0x[0-9a-f]+)', arg)
                if k == 'bss' and mm:
                    size = max(size, int(mm.group(1), 16))
            cdef = [n for n in names if n in defined]
            if cdef:
                by_key[(m.name, addr)] = 'C:' + cdef[0]
                for n in names:
                    if n not in defined:
                        aliases.append((n, cdef[0]))
                continue
            names = [n for n in names if not n.startswith('.')]
            if not names:
                continue
            # The DTCM is 16 KiB at 0x023c0000; its .data is the first 0x20
            # bytes and the rest is the IRQ/SVC/SYS stacks. OS_InitThread
            # writes the stack check words at data_023c0000 + 0x3fc0 and
            # below, so the host storage spans the whole TCM.
            # Only the image's own bytes are filled from the ROM (fill).
            fill = max(size, 4)
            if m.name == 'dtcm' and addr == 0x023c0000:
                size = max(size, 0x4000)
            by_key[(m.name, addr)] = len(storages)
            storages.append(dict(mod=m.name, addr=addr, size=max(size, 4), fill=fill,
                                 section=sec[0], names=names))

    # ---- relocation targets --------------------------------------------------
    sym_index = {}          # module -> (sorted addrs, [(name, kind)])
    for m in mods.values():
        ad, nk = [], []
        for addr, name, kind, arg in m.syms:
            if name.startswith('.'):
                continue
            ad.append(addr)
            nk.append((name, kind, arg))
        sym_index[m.name] = (ad, nk)

    def resolve(mod_name, to):
        ad, nk = sym_index.get(mod_name, ([], []))
        i = bisect.bisect_right(ad, to) - 1
        if i < 0:
            return None
        # several names can share the address: prefer one we can reference
        base = ad[i]
        j = i
        cands = []
        while j >= 0 and ad[j] == base:
            cands.append(nk[j]); j -= 1
        for name, kind, arg in cands:
            key = by_key.get((mod_name, base))
            if kind in ('data', 'bss'):
                if isinstance(key, int):
                    return ('S', key, to - base)
                if isinstance(key, str) and VALID_NAME.match(key[2:]):
                    return ('N', key[2:], to - base)
            if kind in ('function', 'label') and VALID_NAME.match(name):
                return ('N', name, to - base)
        return None

    fills = [i for i, s in enumerate(storages) if s['section'].split('.')[1].startswith(('data', 'rodata'))]
    fill_of = {}
    for i in fills:
        s = storages[i]
        fill_of.setdefault(s['mod'], []).append((s['addr'], s['addr'] + s['size'], i))
    for v in fill_of.values():
        v.sort()

    exact_at = {mn: {ad: nm for ad, nm, k, _ in mm.syms} for mn, mm in mods.items()}
    reloc_from = {mn: {f for f, k, _t, _m, _a in mm.relocs if k == 'load'} for mn, mm in mods.items()}
    # Where a candidate overlay holds an actor profile: a word pointing at one
    # of its own classInit factories (the profile's first field).
    profile_at = {}
    for mn, mm in mods.items():
        if not mn.startswith('ov'):
            continue
        fn = {ad: nm for ad, nm, _k, _x in mm.syms}
        own = 'overlay(%d)' % int(mn[2:])
        profile_at[mn] = {f for f, k, t, tmod, targ in mm.relocs
                          if k == 'load' and (tmod == 'overlay' and str(targ) == str(int(mn[2:]))
                                              or ('%s(%s)' % (tmod, targ)) == own)
                          and 'classInit' in fn.get(t, '')}
    guesses = []            # ambiguous overlay pointers settled by default
    # data_02092208, the stage's level records (dScStage_c sets data_0209f340
    # from it): one pointer per level, each into that level's own overlay,
    # ov(8 + level). All levels' overlays load at the same DS addresses, so
    # the map lists every overlay with something there; picking one for all
    # of them gave the castle's 1F the record of another level, whose model
    # field held a minimap file (the crash entering the castle).
    lt = [ad for ad, nm, _k, _x in mods['main'].syms if nm == 'data_02092208'] if 'main' in mods else []

    def level_table_overlay(mod, frm):
        if mod != 'main' or not lt or not (lt[0] <= frm < lt[0] + 4 * 52):
            return None
        return 'ov%03d' % (8 + (frm - lt[0]) // 4)

    patches = []            # (storage idx, offset, target)
    retargets = []          # (storage idx, offset, overlay id, target): see rim_retargets
    stats = collections.Counter()
    # Raw byte blobs the map holds 'relocations' inside: words in compressed
    # or palette data that happen to look like addresses
    # (port/tools/romimage_raw_data.txt). Patching one would change the bytes,
    # as it did the boot logo's compressed tiles.
    raw = {}
    raw_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'romimage_raw_data.txt')
    if os.path.exists(raw_path):
        names = {l.split('#')[0].strip() for l in open(raw_path)} - {''}
        for mn, mm in mods.items():
            addrs = sorted({ad for ad, _n, _k, _x in mm.syms})
            for ad, nm, _k, _x in mm.syms:
                if nm in names:
                    i = bisect.bisect_right(addrs, ad)
                    raw.setdefault(mn, []).append((ad, addrs[i] if i < len(addrs) else ad + 4))
    for m in mods.values():
        ranges = fill_of.get(m.name, [])
        starts = [r[0] for r in ranges]
        for frm, kind, to, tmod, targ in m.relocs:
            if kind != 'load':
                continue
            if any(lo <= frm < hi for lo, hi in raw.get(m.name, ())):
                stats['inside raw data, not patched'] += 1
                continue
            # Numbers the relocation map mistook for addresses inside a table
            # of numbers (NUMERIC_TABLES): left as they are.
            if any(mod == m.name and lo <= frm < hi for mod, lo, hi, _why in NUMERIC_TABLES):
                stats['inside a numeric table, not patched'] += 1
                continue
            # Two consecutive 16-bit numbers that read as an address -- a
            # tile map's n, n+1 (0x02090208): the title's castle map in ov007
            # had five, patched into host pointers that showed as stray tiles.
            if (to >> 16) in ((to & 0xffff), (to & 0xffff) + 1):
                stats['tile-number pair, not patched'] += 1
                continue
            k = bisect.bisect_right(starts, frm) - 1
            if k < 0 or not (ranges[k][0] <= frm < ranges[k][1]):
                continue
            sidx = ranges[k][2]
            if tmod == 'main' or tmod in ('itcm', 'dtcm'):
                tm = tmod
            elif tmod == 'overlay':
                tm = 'ov%03d' % int(targ)
            elif tmod == 'overlays':
                # The address belongs to whichever overlay is resident when it
                # is used; on the host every overlay has its own storage, so
                # one must be chosen here. In order: the source's own overlay;
                # the one candidate whose symbol AT that address the decomp has
                # named (the game's reference is what got it a name); the
                # overlays resident during play (ov000 common, ov002 player).
                cands = ['ov%03d' % int(x) for x in re.split(r'[,\s]+', targ) if x]
                tm, why = None, None
                lvl = level_table_overlay(m.name, frm)
                if lvl in cands:
                    # the level table: entry k is level k's record, in its
                    # own overlay, 8 + k
                    tm, why = lvl, 'level table'
                elif m.name in cands:
                    tm, why = m.name, 'own overlay'
                else:
                    exact = [c for c in cands if to in exact_at.get(c, {})]
                    named = [c for c in exact if not re.match(r'(data|func)_|@', exact_at[c][to])]
                    # pointer-bearing targets: where the candidate's map has a
                    # pointer AT that address (an actor profile starts with its
                    # factory), a string or a plain table does not
                    linked = [c for c in (named or exact) if to in reloc_from.get(c, set())]
                    profiles = [c for c in cands if to in profile_at.get(c, set())]
                    if len(named) == 1:
                        tm, why = named[0], 'named symbol'
                    elif len(profiles) == 1:
                        # an actor profile (main's spawn table points at
                        # them): the one candidate with a classInit there
                        tm, why = profiles[0], 'actor profile'
                    elif len(linked) == 1:
                        tm, why = linked[0], 'pointer-bearing target'
                    else:
                        pool = named or exact or cands
                        core = [c for c in ('ov000', 'ov002') if c in pool]
                        tm, why = (core[0], 'resident overlay') if core else (pool[0] if pool else None, 'first candidate')
                stats['ambiguous target -> %s' % why] += 1
                if why not in ('own overlay', 'level table'):
                    # the DS's own answer is whichever candidate is resident
                    # when the pointer is used: give the runtime every choice
                    for c in cands:
                        rc = resolve(c, to)
                        if rc is not None:
                            retargets.append((sidx, frm - storages[sidx]['addr'], int(c[2:]), rc))
                if why == 'first candidate':
                    guesses.append('%s 0x%08x -> 0x%08x in %s of %s' % (m.name, frm, to, tm, ','.join(cands)))
            else:
                stats['target module none'] += 1
                continue
            r = resolve(tm, to) if tm else None
            if r is None:
                stats['unresolved target'] += 1
                continue
            patches.append((sidx, frm - storages[sidx]['addr'], r))
            stats['patch'] += 1

    # ACTOR_SPAWN_TABLE entries the map records no relocation for (four in the
    # EUR map: RESULT, BK_UKISIMA, BATTA_BLOCK, PICTURE_GATE): the entry is
    # still a profile pointer, and symbols/actor_debug_names.tsv plus the
    # g_profile_<NAME> symbol name it.
    names_tsv = os.path.join(os.path.dirname(a.config.rstrip('/')), '..', 'symbols', 'actor_debug_names.tsv')
    names_tsv = os.path.normpath(names_tsv)
    spawn = [(ad, nm) for ad, nm, k, _ in mods['main'].syms if nm == 'ACTOR_SPAWN_TABLE']
    key = by_key.get(('main', spawn[0][0])) if spawn else None
    if spawn and isinstance(key, int) and os.path.exists(names_tsv):
        base = spawn[0][0]
        have = {off for si, off, _r in patches if si == key}
        prof = {}
        for mn, mm in mods.items():
            for ad, nm, k, _ in mm.syms:
                if nm.startswith('g_profile_'):
                    prof.setdefault(nm, []).append((mn, ad))
        for l in open(names_tsv):
            parts = l.rstrip('\n').split('\t')
            if len(parts) < 2 or not parts[0].isdigit():
                continue
            aid, name = int(parts[0]), parts[1]
            off = 4 * aid
            if off in have or off + 4 > storages[key]['size']:
                continue
            hits = prof.get('g_profile_' + name, [])
            if len(hits) != 1:
                continue
            r = resolve(hits[0][0], hits[0][1])
            if r is not None:
                patches.append((key, off, r))
                stats['spawn-table entry named by profile'] += 1

    # A DS pointer to something the host binary does not have (the DS's static
    # initializers __sinit_*, which the host C++ runtime replaces; the DS C++
    # runtime's own RTTI classes) is written as null rather than linked: the
    # host never follows it, and the list says which ones they are.
    nulled = sorted({r[1] for _, _, r in patches if r[0] == 'N' and r[1] not in defined})
    if nulled:
        patches = [(si, off, (('Z', None, 0) if (r[0] == 'N' and r[1] not in defined) else r))
                   for si, off, r in patches]
    with open(a.out_ld + '.nulled.txt', 'w') as g:
        g.write('# DS pointers to symbols the host does not define: written as null\n')
        g.writelines(n + '\n' for n in nulled)
    stats['pointer target not on host (nulled)'] = len(nulled)

    # ---- which module each game TU belongs to (its static initializers run
    # when that module loads: the ARM9's at boot, an overlay's at FS_LoadOverlay)
    init_mods = []
    if a.out_init_ld and a.sources and a.repo:
        import glob as _g, json as _j
        sym_mod = {}
        for mn, mm in mods.items():
            for ad, nm, k, _ in mm.syms:
                sym_mod.setdefault(nm, mn)
        src_mod = {}
        if a.manifests:
            for f in _g.glob(os.path.join(a.manifests, '**', '*.json'), recursive=True):
                try:
                    d = _j.load(open(f))
                except ValueError:
                    continue
                mn = d.get('module')
                srcp = d.get('promoted_source') or d.get('source')
                if mn and srcp:
                    src_mod[srcp] = 'main' if mn == 'arm9' else mn
        tus = [l.strip() for l in open(a.sources) if l.strip() and not l.startswith('#')]
        groups = collections.OrderedDict()
        for tu in tus:
            mn = src_mod.get(tu)
            if not mn:
                try:
                    text = open(os.path.join(a.repo, tu), errors='replace').read()
                except OSError:
                    text = ''
                votes = collections.Counter(sym_mod.get(n) for n in re.findall(r'@symbol\s+(\S+)', text))
                votes.pop(None, None)
                mn = votes.most_common(1)[0][0] if votes else 'main'
            if mn in ('itcm', 'dtcm'):
                mn = 'main'
            groups.setdefault(mn, []).append(tu)
        init_mods = list(groups)
        with open(a.out_init_ld, 'w') as o:
            o.write('/* GENERATED by port/tools/romimage.py: the game\'s C++ static initializers,\n'
                    ' * one output section per DS module, pulled out of .init_array so the C\n'
                    ' * runtime does not run them before main. The port runs a module\'s when\n'
                    ' * the game loads it (port/hal/romimage_rt.cpp). */\nSECTIONS\n{\n')
            for mn, files in groups.items():
                o.write('    .sm64ds_init_%s : ALIGN(4)\n    {\n' % mn)
                o.write('        PROVIDE_HIDDEN(__sm64ds_init_%s_start = .);\n' % mn)
                for tu in files:
                    stem = os.path.splitext(tu)[0]
                    # src/x.c builds as x.c.o(bj), or from its host copy x.cpp.o(bj)
                    o.write('        KEEP(*/%s.c*.o*(.init_array .init_array.*))\n' % stem)
                o.write('        PROVIDE_HIDDEN(__sm64ds_init_%s_end = .);\n    }\n' % mn)
            o.write('}\nINSERT BEFORE .init_array;\n')
        stats['TUs by module for static init: %d modules' % len(groups)] = len(tus)

    # ---- emit ---------------------------------------------------------------
    retargets = [t for t in retargets if not (t[3][0] == 'N' and t[3][1] not in defined)]
    ext_names = sorted({r[1] for _, _, r in patches if r[0] == 'N'}
                       | {t[3][1] for t in retargets if t[3][0] == 'N'})
    with open(a.out_cpp, 'w') as o:
        o.write('// GENERATED by port/tools/romimage.py from config/arm9 maps. Do not edit.\n')
        o.write('// Storage for the game globals that live only in the ROM image, and the\n')
        o.write('// tables port/hal/romimage_rt.cpp fills them from at startup. No ROM bytes.\n')
        o.write('#include "romimage_rt.h"\n\n')
        # Storages that sit back to back on the DS (one struct the maps split
        # at its interior labels -- OSi_ThreadInfo is data_020a6134 with
        # data_020a613c and data_020a6148 inside it) share one block, laid out
        # exactly as on the DS, so an access past one name lands where the DS
        # put it. Each storage is a fixed offset into its block; a block starts
        # 32-byte aligned at its first DS address rounded down to 32, so every
        # storage keeps its DS alignment.
        order = sorted(range(len(storages)), key=lambda i: (storages[i]['mod'], storages[i]['section'], storages[i]['addr']))
        blocks = []             # [mod, section, base, end, [storage idx]]
        for i in order:
            st = storages[i]
            b = blocks[-1] if blocks else None
            if b and b[0] == st['mod'] and b[1] == st['section'] and st['addr'] <= b[3]:
                b[3] = max(b[3], st['addr'] + st['size'])
                b[4].append(i)
            else:
                blocks.append([st['mod'], st['section'], st['addr'] & ~31, st['addr'] + st['size'], [i]])
        for bi, (mn, secname, base, end, members) in enumerate(blocks):
            sec = '.bss' if secname.startswith('.bss') else '.data'
            first = storages[members[0]]['names'][0]
            tag = first if VALID_NAME.match(first) else 'blk%d' % bi
            o.write('alignas(32) static unsigned char rimblk_%d[%d] __asm__("sm64ds_rimblk_%d") '
                    '__attribute__((used, section("%s.rim.%s")));\n' % (bi, end - base, bi, sec, tag))
            for i in members:
                st = storages[i]
                off = st['addr'] - base
                o.write('#define %s (rimblk_%d + %d)\n' % (c_ident(i), bi, off))
                if VALID_NAME.match(st['names'][0]):
                    # A vtable kept from the ROM: the maps put _ZTV at the
                    # address point (the first slot; offset-to-top and the
                    # type info are the two words before it), where GCC's
                    # code puts _ZTV two words BEFORE the address point and
                    # stores _ZTV + 8 as the vptr. The name goes 8 bytes
                    # down, so a host constructor's vptr lands on the ROM's
                    # first slot: without it the weather's clipCallback_c
                    # called a null SpawnParticles (Cool, Cool Mountain).
                    nm = st['names'][0]
                    sym_off = off - 8 if nm.startswith('_ZTV') else off
                    o.write('__asm__(".globl %s\\n.type %s, %%object\\n.size %s, %d\\n.set %s, sm64ds_rimblk_%d %s %d");\n'
                            % ((nm,) * 3 + (st['size'] + (8 if sym_off != off else 0), nm, bi,
                                           '-' if sym_off < 0 else '+', abs(sym_off))))
        stats['storage blocks'] = len(blocks)
        o.write('\n')
        priv_f = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'vita', 'game_private_names.txt')
        priv = set()
        if os.path.exists(priv_f):
            priv = {l.strip() for l in open(priv_f) if l.strip() and not l.startswith('#')}
        for i, n in enumerate(ext_names):
            o.write('extern unsigned char rimx_%d __asm__("%s");\n'
                    % (i, ('sm64ds_game_' + n) if n in priv else n))
        xid = {n: i for i, n in enumerate(ext_names)}
        o.write('\nconst RimFill rim_fills[] = {\n')
        for i in fills:
            s = storages[i]
            mid = -1 if s['mod'] == 'main' else (-2 if s['mod'] == 'itcm' else (-3 if s['mod'] == 'dtcm' else int(s['mod'][2:])))
            o.write('  {%d, 0x%08x, %d, %s},\n' % (mid, s['addr'], s.get('fill', s['size']), c_ident(i)))
        o.write('};\nconst unsigned rim_fill_count = sizeof rim_fills / sizeof rim_fills[0];\n\n')
        o.write('const RimPatch rim_patches[] = {\n')
        for sidx, off, r in patches:
            if r[0] == 'Z':
                o.write('  {%s + %d, 0},\n' % (c_ident(sidx), off))
                continue
            tgt = ('%s' % c_ident(r[1])) if r[0] == 'S' else ('&rimx_%d' % xid[r[1]])
            o.write('  {%s + %d, %s + %d},\n' % (c_ident(sidx), off, tgt, r[2]))
        o.write('};\nconst unsigned rim_patch_count = sizeof rim_patches / sizeof rim_patches[0];\n')
        o.write('\nconst RimRetarget rim_retargets[] = {\n')
        for sidx, off, ov, r in retargets:
            if r[0] == 'N' and r[1] not in xid:
                continue
            tgt = ('%s' % c_ident(r[1])) if r[0] == 'S' else ('&rimx_%d' % xid[r[1]])
            o.write('  {%s + %d, %d, %s + %d},\n' % (c_ident(sidx), off, ov, tgt, r[2]))
        o.write('  {0, -99, 0}\n};\nconst unsigned rim_retarget_count = sizeof rim_retargets / sizeof rim_retargets[0] - 1;\n')
        def mod_id(mn):
            return -1 if mn == 'main' else (-2 if mn == 'itcm' else (-3 if mn == 'dtcm' else int(mn[2:])))
        o.write('\nconst RimBss rim_bss[] = {\n')
        for i, st in enumerate(storages):
            if st['section'].startswith('.bss'):
                o.write('  {%d, %s, %d},\n' % (mod_id(st['mod']), c_ident(i), st['size']))
        o.write('  {-99, 0, 0}\n};\nconst unsigned rim_bss_count = sizeof rim_bss / sizeof rim_bss[0] - 1;\n')
        if not a.out_init_ld:
            o.write('const RimInit rim_inits[] = {{-99, 0, 0}};\nconst unsigned rim_init_count = 0;\n')
        else:
            o.write('\n// each module\'s static initializers (grouped by %s)\n' % os.path.basename(a.out_init_ld))
            for mn in init_mods:
                o.write('extern "C" void (*const __sm64ds_init_%s_start[])(void);\n' % mn)
                o.write('extern "C" void (*const __sm64ds_init_%s_end[])(void);\n' % mn)
            o.write('const RimInit rim_inits[] = {\n')
            for mn in init_mods:
                o.write('  {%d, __sm64ds_init_%s_start, __sm64ds_init_%s_end},\n' % (mod_id(mn), mn, mn))
            o.write('  {-99, 0, 0}\n};\nconst unsigned rim_init_count = sizeof rim_inits / sizeof rim_inits[0] - 1;\n')
    with open(a.out_ld, 'w') as o:
        o.write('/* GENERATED by port/tools/romimage.py: other names of C-defined globals */\n')
        seen = set()
        for n, t in aliases:
            if n in seen or not VALID_NAME.match(n) or not VALID_NAME.match(t):
                continue
            seen.add(n)
            o.write('PROVIDE("%s" = "%s");\n' % (n, t))
        for i, s in enumerate(storages):
            if not VALID_NAME.match(s['names'][0]):
                continue
            for n in s['names'][1:]:
                if n not in seen and VALID_NAME.match(n):
                    seen.add(n)
                    o.write('PROVIDE("%s" = "%s");\n' % (n, s['names'][0]))
    # ---- names a TU references but the retail link resolves by address --------
    # (per-actor resource-family types and the like: config/tu_manifest.d
    # records, per relocation site, which canonical symbol the ROM calls)
    n_man = n_unres = 0
    if a.manifests:
        import glob, json
        with open(a.out_ld, 'a') as o:
            for f in sorted(glob.glob(os.path.join(a.manifests, '**', '*.json'), recursive=True)):
                try:
                    d = json.load(open(f))
                except ValueError:
                    continue
                for al in d.get('undefined_symbol_aliases') or []:
                    n, t = al.get('symbol'), al.get('canonical_symbol')
                    if not n or not t or n in seen or n in defined:
                        continue
                    if not VALID_NAME.match(n):
                        continue
                    # The canonical NAME can be stale (renamed since); its
                    # address is not. Take whichever name at that address the
                    # objects actually define.
                    if t not in defined:
                        mod = al.get('canonical_module') or 'main'
                        mod = 'main' if mod == 'arm9' else mod
                        try:
                            addr = int(al.get('canonical_address', '0'), 16)
                        except ValueError:
                            addr = 0
                        alts = [nm for ad_, nm, k, _ in mods.get(mod, mods['main']).syms
                                if ad_ == addr and nm in defined]
                        if not alts:
                            n_unres += 1
                            continue
                        t = alts[0]
                    if not VALID_NAME.match(t):
                        continue
                    seen.add(n)
                    o.write('PROVIDE("%s" = "%s");\n' % (n, t))
                    n_man += 1
    # OVERLAY_<n>_ID and OV<nnn>_TEXT_START/_BSS_END: the DS link's per-overlay
    # constants, used by address. The bounds stay DS addresses (the game
    # compares and subtracts them; it never dereferences them on the host).
    with open(a.out_ld, 'a') as o:
        for name, mm in mods.items():
            if not name.startswith('ov'):
                continue
            n = int(name[2:])
            o.write('PROVIDE(OVERLAY_%d_ID = %d);\n' % (n, n))
            text = [s_ for s_ in mm.sections if s_[0] == '.text']
            bss = [s_ for s_ in mm.sections if s_[0].startswith('.bss')]
            if text:
                o.write('PROVIDE(OV%03d_TEXT_START = 0x%08x);\n' % (n, text[0][1]))
            if bss:
                o.write('PROVIDE(OV%03d_BSS_END = 0x%08x);\n' % (n, bss[-1][2]))
    # Hand-checked aliases the maps cannot derive (port/vita/host_aliases.ld)
    extra = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'vita', 'host_aliases.ld')
    if os.path.exists(extra):
        with open(a.out_ld, 'a') as o:
            o.write(open(extra).read())
    print('romimage: %d manifest aliases (%d with no defined target)' % (n_man, n_unres), file=sys.stderr)
    with open(a.out_ld + '.guesses.txt', 'w') as g:
        g.write('# overlay pointers with no naming evidence: target chosen by default\n')
        g.writelines(x + '\n' for x in guesses)
    print('romimage: %d storages (%d filled, %d bss, %d KiB), %d patches, %d aliases'
          % (len(storages), len(fills), len(storages) - len(fills),
             sum(s['size'] for s in storages) // 1024, len(patches), len(seen)), file=sys.stderr)
    for k, v in sorted(stats.items()):
        if k != 'patch':
            print('romimage:   %s: %d' % (k, v), file=sys.stderr)


if __name__ == '__main__':
    main()
