#!/usr/bin/env python3
"""Write gw_fpga_firmware.h with the FPGA_FIRMWARE bitstream taken from a local stock gw-fpga.ko.

The bitstream is FiiO firmware and is not kept in the repo (the checked-in header is a zero placeholder).
Usage: extract-gw-fpga-firmware.py STOCK_GW_FPGA_KO OUT_HEADER
"""
import struct
import sys


def main(ko, out):
    d = open(ko, 'rb').read()
    shoff, = struct.unpack_from('<Q', d, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x3a)
    secs = [struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * shentsize) for i in range(shnum)]
    for s in secs:
        if s[1] != 2:  # SHT_SYMTAB
            continue
        strtab = secs[s[6]]
        for off in range(s[4], s[4] + s[5], 24):
            name, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', d, off)
            p = strtab[4] + name
            if d[p:d.index(b'\0', p)] != b'FPGA_FIRMWARE':
                continue
            blob = d[secs[shndx][4] + value:secs[shndx][4] + value + size]
            with open(out, 'w') as f:
                f.write('/* Gowin bitstream built into the stock gw-fpga.ko (extracted at build time) */\n')
                f.write('#include <linux/types.h>\n\n')
                f.write('const uint8_t FPGA_FIRMWARE[%d] = {\n' % size)
                for i in range(0, size, 16):
                    f.write('\t' + ', '.join('0x%02x' % b for b in blob[i:i + 16]) + ',\n')
                f.write('};\n')
            return 0
    sys.exit('FPGA_FIRMWARE not found in ' + ko)


if __name__ == '__main__':
    sys.exit(main(*sys.argv[1:3]))
