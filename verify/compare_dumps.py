#!/usr/bin/env python3
"""
compare_dumps.py - compare two raw section dumps of the same PE module taken from
two different runs of the game (as produced by verifydump.dll), while ignoring
every byte the Windows loader is allowed to change between runs.

What is ignored, and why:

  * every address listed in the .reloc directory - the loader patches those when
    the module is mapped at a different base address (ASLR), so they legitimately
    differ between two runs.  This is what makes a naive byte comparison of .text
    or .rdata useless.
  * the import address table (the FirstThunk arrays of every import descriptor) -
    those hold resolved pointers to functions inside other DLLs, which are mapped
    at different addresses in every run.

Everything else must be byte identical.  Any remaining difference is a genuine
change made by whatever was loaded in one run and not in the other, and is
reported with its section and RVA.

usage:
  compare_dumps.py <pe-file> <sectionA.bin> <sectionB.bin> [more pairs...]
    e.g. compare_dumps.py bm2dx.dll A_text.bin B_text.bin A_rdata.bin B_rdata.bin
"""

import struct
import sys


def parse_pe(path):
    data = open(path, 'rb').read()
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    assert data[pe:pe + 4] == b'PE\0\0', 'not a PE file'
    nsec = struct.unpack_from('<H', data, pe + 6)[0]
    opt_size = struct.unpack_from('<H', data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from('<H', data, opt)[0]
    ddir = opt + (0x70 if magic == 0x20B else 0x60)
    sections = []
    for i in range(nsec):
        o = opt + opt_size + i * 40
        name = data[o:o + 8].rstrip(b'\0').decode('ascii', 'replace')
        vsize, vaddr, rsize, raddr = struct.unpack_from('<IIII', data, o + 8)
        sections.append(dict(name=name, vaddr=vaddr,
                             vsize=vsize if vsize else rsize,
                             raddr=raddr, rsize=rsize))
    result = dict(sections=sections, relocs=[], iat=[])

    def rva_to_off(rva):
        for s in sections:
            if s['vaddr'] <= rva < s['vaddr'] + max(s['vsize'], s['rsize']):
                return s['raddr'] + (rva - s['vaddr'])
        return None

    # ---- base relocations
    rel_rva, rel_size = struct.unpack_from('<II', data, ddir + 5 * 8)
    rel_off = rva_to_off(rel_rva) if rel_rva else None
    if rel_off is not None and rel_size:
        blob = data[rel_off:rel_off + rel_size]
        o = 0
        while o + 8 <= len(blob):
            page, block = struct.unpack_from('<II', blob, o)
            if block < 8:
                break
            n = (block - 8) // 2
            for k in range(n):
                ent = struct.unpack_from('<H', blob, o + 8 + k * 2)[0]
                typ, off = ent >> 12, ent & 0xFFF
                if typ == 10:
                    result['relocs'].append((page + off, 8))
                elif typ == 3:
                    result['relocs'].append((page + off, 4))
            o += block

    # ---- import address tables
    imp_rva, imp_size = struct.unpack_from('<II', data, ddir + 1 * 8)
    imp_off = rva_to_off(imp_rva) if imp_rva else None
    if imp_off is not None:
        o = imp_off
        while True:
            oft, stamp, fwd, name_rva, first = struct.unpack_from('<IIIII', data, o)
            if name_rva == 0 and first == 0:
                break
            name_off = rva_to_off(name_rva)
            if name_off is None:
                break
            dll = data[name_off:data.index(b'\0', name_off)].decode('ascii', 'replace')
            count = 0
            src = oft if oft else first
            src_off = rva_to_off(src)
            if src_off is None:
                break
            while True:
                thunk = struct.unpack_from('<Q', data, src_off + count * 8)[0]
                if thunk == 0:
                    break
                count += 1
            result['iat'].append(dict(dll=dll, rva=first, count=count))
            o += 20
    return result


def mask_for(pe, section):
    """byte ranges to ignore inside one section"""
    lo, hi = section['vaddr'], section['vaddr'] + section['vsize']
    ranges = []
    for rva, size in pe['relocs']:
        if lo <= rva < hi:
            ranges.append((rva - lo, size))
    for imp in pe['iat']:
        start = imp['rva']
        end = start + imp['count'] * 8
        if start < hi and end > lo:
            ranges.append((max(start, lo) - lo, min(end, hi) - max(start, lo)))
    return ranges


def build_mask(length, ranges):
    mask = bytearray(length)
    for off, size in ranges:
        for i in range(off, min(off + size, length)):
            mask[i] = 1
    return mask


def main():
    pe_path = sys.argv[1]
    pairs = sys.argv[2:]
    pe = parse_pe(pe_path)
    by_name = {s['name']: s for s in pe['sections']}
    print('module sections : ' + ', '.join(f"{s['name']}(v=0x{s['vaddr']:X},r=0x{s['vsize']:X})"
                                           for s in pe['sections']))
    print('relocations     : %d entries' % len(pe['relocs']))
    print('import tables   : ' + ', '.join(f"{i['dll']}({i['count']})" for i in pe['iat']))
    print()

    total_left = 0
    for i in range(0, len(pairs), 2):
        a_path, b_path = pairs[i], pairs[i + 1]
        name = a_path.split('_')[-1].replace('.bin', '')
        section = by_name.get('.' + name)
        if section is None:
            print(f'!! section .{name} not found in {pe_path}')
            continue
        a = open(a_path, 'rb').read()
        b = open(b_path, 'rb').read()
        if len(a) != len(b):
            print(f'!! {name}: size mismatch {len(a)} vs {len(b)}')
            continue
        mask = build_mask(len(a), mask_for(pe, section))
        runs = []
        i2 = 0
        while i2 < len(a):
            if mask[i2] or a[i2] == b[i2]:
                i2 += 1
                continue
            start = i2
            while i2 < len(a) and not mask[i2] and a[i2] != b[i2]:
                i2 += 1
            runs.append((start, i2 - start))
        left = sum(r[1] for r in runs)
        total_left += left
        ignored = sum(mask)
        print(f"=== {name}: {len(a)} bytes, ignored(loader)={ignored}, "
              f"differences after masking={left} in {len(runs)} run(s) ===")
        for off, ln in runs[:20]:
            rva = section['vaddr'] + off
            print(f"    rva=0x{rva:08X} len={ln}  A={a[off:off+min(ln,12)].hex()} "
                  f"B={b[off:off+min(ln,12)].hex()}")
        if len(runs) > 20:
            print(f"    ... and {len(runs)-20} more")
        print()

    print('=== verdict ===')
    if total_left == 0:
        print('PASS: after ignoring loader written bytes (relocations + import table)')
        print('      the two runs are byte identical.')
    else:
        print(f'FAIL: {total_left} byte(s) differ that the loader does not explain.')
    return 0 if total_left == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
