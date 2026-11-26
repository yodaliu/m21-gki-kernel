#!/usr/bin/env python3
"""Compare rebuilt vendor modules against the stock FW 1.1.0 modules.

usage: compare-vendor-modules.py STOCK_DIR BUILT_DIR [--json OUT] [--readelf PATH]

STOCK_DIR and BUILT_DIR are searched recursively for *.ko.  Modules are paired
by module name (file name with '-' == '_').  For every pair the script compares
what can be compared without source code:

  funcs    defined FUNC symbols and their sizes (CFI jump-table stubs ignored)
  exports  symbols the module exports (__ksymtab_*)
  imports  undefined symbols the module needs from the kernel/other modules
  strings  printable strings of .rodata* sections (log messages, names);
           build paths are ignored because they name the build machine

Verdicts:
  IDENTICAL  same functions with the same sizes, same exports/imports/strings
  CLOSE      >=95% of functions present with equal size and exports/imports equal
  DIFFERENT  built, but differs more than that
  MISSING    in stock, not produced by the rebuild

The toolchain is the same clang r450784e the stock build used, so equal source
normally gives equal function sizes.  Equal sizes are strong evidence, not a
byte-for-byte proof.  Exit code is always 0; read the summary.
"""
import argparse
import json
import os
import re
import struct
import subprocess
import sys

CFI_RE = re.compile(r'(\.cfi(_jt)?$|^__cfi_|^__typeid_|\.llvm\.\d+$)')


def find_ko(root):
    out = {}
    for d, _, files in os.walk(root):
        for f in files:
            if f.endswith('.ko'):
                out.setdefault(f[:-3].replace('-', '_'), os.path.join(d, f))
    return out


def symbols(readelf, path):
    txt = subprocess.run([readelf, '-sW', path], capture_output=True, text=True).stdout
    funcs, exports, imports = {}, set(), set()
    for line in txt.splitlines():
        p = line.split()
        if len(p) < 8 or not p[0].endswith(':'):
            continue
        size, typ, ndx, name = p[2], p[3], p[6], p[7]
        if ndx == 'UND':
            if name:
                imports.add(name)
            continue
        if name.startswith('__ksymtab_'):
            exports.add(name[len('__ksymtab_'):])
        if typ == 'FUNC' and not CFI_RE.search(name):
            funcs[re.sub(r'\.llvm\.\d+$', '', name)] = int(size, 0) if size.startswith('0x') else int(size)
    return funcs, exports, imports


def rodata_bytes(path):
    """Concatenated contents of all .rodata* sections (ELF64 little-endian)."""
    d = open(path, 'rb').read()
    if d[:4] != b'\x7fELF' or d[4] != 2:
        return b''
    shoff, = struct.unpack_from('<Q', d, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x3a)
    secs = [struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * shentsize) for i in range(shnum)]
    stroff = secs[shstrndx][4]
    out = []
    for name, typ, _, _, off, size, *_ in secs:
        end = d.index(b'\0', stroff + name)
        sname = d[stroff + name:end].decode(errors='replace')
        if sname.startswith('.rodata') and typ == 1:  # SHT_PROGBITS
            out.append(d[off:off + size])
    return b'\0'.join(out)


def rodata_strings(path, minlen=6):
    data = rodata_bytes(path)
    found = set()
    for m in re.finditer(rb'[\x20-\x7e]{%d,}' % minlen, data):
        s = m.group().decode()
        # source file names (__FILE__) keep only the basename: the directory
        # prefix depends on where the tree was checked out, not on the source
        if '/' in s and re.search(r'\.(c|h)$', s):
            s = 'FILE:' + s.rsplit('/', 1)[-1]
        elif '/' in s and ('kernel_platform' in s or s.startswith('/')):
            continue
        found.add(s)
    return found


def compare(readelf, stock, built):
    sf, se, si = symbols(readelf, stock)
    bf, be, bi = symbols(readelf, built)
    names = set(sf) | set(bf)
    same_size = sum(1 for n in sf if n in bf and bf[n] == sf[n])
    ss, bs = rodata_strings(stock), rodata_strings(built)
    r = {
        'funcs_stock': len(sf),
        'funcs_same_size': same_size,
        'funcs_only_stock': sorted(set(sf) - set(bf))[:20],
        'funcs_only_built': sorted(set(bf) - set(sf))[:20],
        'funcs_size_diff': sorted(n for n in sf if n in bf and bf[n] != sf[n])[:20],
        'exports_equal': se == be,
        'imports_only_stock': sorted(si - bi)[:20],
        'imports_only_built': sorted(bi - si)[:20],
        'strings_only_stock': len(ss - bs),
        'strings_only_built': len(bs - ss),
    }
    ratio = same_size / len(sf) if sf else 1.0
    r['func_match'] = round(ratio, 4)
    if (same_size == len(sf) == len(bf) and se == be and si == bi and ss == bs):
        r['verdict'] = 'IDENTICAL'
    elif ratio >= 0.95 and se == be and not (si ^ bi):
        r['verdict'] = 'CLOSE'
    else:
        r['verdict'] = 'DIFFERENT'
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('stock')
    ap.add_argument('built')
    ap.add_argument('--json')
    ap.add_argument('--readelf', default=os.environ.get('READELF', 'llvm-readelf'))
    ap.add_argument('--only', help='file with module names to consider (one per line)')
    a = ap.parse_args()

    stock, built = find_ko(a.stock), find_ko(a.built)
    if not stock:
        sys.exit(f'no .ko under {a.stock}')
    only = None
    if a.only:
        only = {l.strip().replace('-', '_') for l in open(a.only) if l.strip() and not l.startswith('#')}
    res = {}
    for name in sorted(stock):
        if only is not None and name not in only:
            continue
        if name not in built:
            res[name] = {'verdict': 'MISSING'}
        else:
            res[name] = compare(a.readelf, stock[name], built[name])
    counts = {}
    for v in res.values():
        counts[v['verdict']] = counts.get(v['verdict'], 0) + 1
    width = max(len(n) for n in res) if res else 10
    for n, v in res.items():
        extra = ''
        if v['verdict'] in ('CLOSE', 'DIFFERENT'):
            extra = (f"funcs {v['funcs_same_size']}/{v['funcs_stock']} same-size, "
                     f"exports {'=' if v['exports_equal'] else '!='}, "
                     f"imports -{len(v['imports_only_stock'])}/+{len(v['imports_only_built'])}, "
                     f"strings -{v['strings_only_stock']}/+{v['strings_only_built']}")
        print(f'{n:<{width}}  {v["verdict"]:<9}  {extra}')
    print('SUMMARY: ' + ', '.join(f'{k}={counts[k]}' for k in sorted(counts)) + f', total={len(res)}')
    if a.json:
        with open(a.json, 'w') as f:
            json.dump({'summary': counts, 'modules': res}, f, indent=1, sort_keys=True)


if __name__ == '__main__':
    main()
