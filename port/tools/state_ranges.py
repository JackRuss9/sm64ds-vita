#!/usr/bin/env python3
"""Save-state ranges from gate 11's link map (port/vita/savestate_host.cpp).

A save state holds the game's own globals and none of the port's. This reads
the GNU ld map (-Wl,-Map) and collects every .data*/.bss* input section that
came from one of the game's objects -- everything in gate10_game except the
port's own files there -- and from the ROM-image storage (romimage_gen),
merges neighbours, and writes them relative to an anchor (one game global,
data_0209d50c) so they hold wherever the module is loaded:

    "SMDSRNG1", u32 count, u32 anchor address (link time),
    then count x (u32 address, u32 size), link-time addresses.

Usage: state_ranges.py gate11.map state_ranges.bin
"""
import re
import struct
import sys

ANCHOR = 'data_0209d50c'
# the port's own files inside gate10_game: not game state
PORT_FILES = ('game_host.cpp', 'os_thread_host.cpp', 'irq_host.cpp', 'arena_host.cpp',
              'overlay_host.cpp', 'timers_host.cpp', 'hal/fs.cpp', 'hal/romfs.cpp',
              'hal/romimage_rt.cpp')

SEC = re.compile(r'^ (\.data\S*|\.bss\S*|COMMON)\s*(?:(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+(\S.*))?$')
NEXT = re.compile(r'^\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+(\S.*)$')
SYM = re.compile(r'^\s+(0x[0-9a-fA-F]+)\s+(\S+)\s*$')


def game_object(path):
    p = path.replace('\\', '/')
    if 'romimage_gen.cpp' in p:
        return True
    if 'gate10_game.dir/' not in p:
        return False
    return not any(p.endswith(f + '.o') or p.endswith(f + '.obj') for f in PORT_FILES)


def main():
    lines = open(sys.argv[1], errors='replace').read().splitlines()
    ranges = []
    anchor = None
    pending = None
    for ln in lines:
        if pending is not None:
            m = NEXT.match(ln)
            if m:
                addr, size, obj = int(m.group(1), 16), int(m.group(2), 16), m.group(3).strip()
                if size and addr and game_object(obj) and '.rel.ro' not in pending:
                    ranges.append((addr, size))
            pending = None
            continue
        m = SEC.match(ln)
        if m:
            name = m.group(1)
            if m.group(2):
                addr, size, obj = int(m.group(2), 16), int(m.group(3), 16), m.group(4).strip()
                if size and addr and game_object(obj) and '.rel.ro' not in name:
                    ranges.append((addr, size))
            else:
                pending = name
            continue
        m = SYM.match(ln)
        if m and m.group(2) == ANCHOR:
            anchor = int(m.group(1), 16)
    if anchor is None:
        sys.exit('state_ranges: anchor %s not in the map' % ANCHOR)
    ranges.sort()
    merged = []
    for a, n in ranges:
        if merged and a <= merged[-1][0] + merged[-1][1] + 16:   # neighbours (alignment gaps)
            end = max(merged[-1][0] + merged[-1][1], a + n)
            merged[-1] = (merged[-1][0], end - merged[-1][0])
        else:
            merged.append((a, n))
    with open(sys.argv[2], 'wb') as o:
        o.write(b'SMDSRNG1')
        o.write(struct.pack('<II', len(merged), anchor))
        for a, n in merged:
            o.write(struct.pack('<II', a, n))
    total = sum(n for _, n in merged)
    print('state_ranges: %d sections in %d ranges, %d KB, anchor %s at 0x%x'
          % (len(ranges), len(merged), total // 1024, ANCHOR, anchor))


if __name__ == '__main__':
    main()
