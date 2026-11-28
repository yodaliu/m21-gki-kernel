#!/usr/bin/env python3
"""Write the hyn_chips/*_fw.h headers of hynitron_touch with the firmware taken from a local stock hynitron_touch.ko.

The touch firmware is Hynitron/FiiO data and is not kept in the repo.
Each array is found in the stock symbol table by its name (without the ".N" suffix clang adds to
static symbols) and its size; names, element types, sizes and headers come from the stock DWARF.
Arrays the stock build optimized away (unused) are written as zeros of the stock size.
Usage: extract-hynitron-firmware.py STOCK_HYNITRON_TOUCH_KO|- OUT_HYN_CHIPS_DIR
("-": no stock module, every array is written as zeros so the module still builds)
"""
import os
import struct
import sys

# header: [(variable, element type, size, present in the stock module)]
FW = {
    'cst226se_fw.h': [('fw_bin', 'uint8_t', 7680, True)],
    'cst3240_fw.h': [('fw_bin', 'unsigned char', 16352, True)],
    'cst36xxes_fw.h': [('fw', 'uint8_t', 10240, True), ('fw2', 'uint8_t', 10240, True)],
    'cst3xx_fw.h': [('fw_bin', 'uint8_t', 24600, True)],
    'cst66xx_fw1.h': [('fw_module1', 'uint8_t', 40964, True)],
    'cst66xx_fw2.h': [('fw_module2', 'uint8_t', 40964, True)],
    'cst76xx_fw.h': [('fw_module', 'uint8_t', 77824, True)],
    'cst7xx_fw.h': [('fw_bin_1', 'unsigned char', 15366, True), ('fw_bin_2', 'unsigned char', 6, False),
                    ('fw_bin_3', 'unsigned char', 6, False)],
    'cst840u_fw.h': [('fw_module', 'uint8_t', 45060, True)],
    'cst8xxT_fw.h': [('fw_bin', 'unsigned char', 15366, True)],
    'cst923xx_fw.h': [('fw_bin0', 'u8', 32256, True), ('fw_bin1', 'unsigned char', 32640, False)],
}


def symbols(d):
    shoff, = struct.unpack_from('<Q', d, 0x28)
    shentsize, shnum = struct.unpack_from('<HH', d, 0x3a)
    secs = [struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * shentsize) for i in range(shnum)]
    out = {}
    for s in secs:
        if s[1] != 2:  # SHT_SYMTAB
            continue
        strtab = secs[s[6]]
        for off in range(s[4], s[4] + s[5], 24):
            name, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', d, off)
            if info & 0xf != 1 or not 0 < shndx < shnum:  # STT_OBJECT
                continue
            p = strtab[4] + name
            n = d[p:d.index(b'\0', p)].decode().split('.')[0]
            out.setdefault((n, size), []).append(d[secs[shndx][4] + value:secs[shndx][4] + value + size])
    return out


def main(ko, outdir):
    syms = None if ko == '-' else symbols(open(ko, 'rb').read())
    for hdr, arrs in FW.items():
        with open(os.path.join(outdir, hdr), 'w') as f:
            f.write('/* firmware built into the stock hynitron_touch.ko (extracted at build time) */\n')
            for var, ctype, size, present in arrs:
                if present and syms is not None:
                    blobs = syms.get((var, size), [])
                    if len(blobs) != 1:
                        sys.exit('%s: %d stock symbols named %s of %d bytes' % (ko, len(blobs), var, size))
                    blob = blobs[0]
                else:
                    blob = bytes(size)
                f.write('static const %s %s[%d] = {\n' % (ctype, var, size))
                for i in range(0, size, 16):
                    f.write('\t' + ', '.join('0x%02x' % b for b in blob[i:i + 16]) + ',\n')
                f.write('};\n')
    return 0


if __name__ == '__main__':
    sys.exit(main(*sys.argv[1:3]))
